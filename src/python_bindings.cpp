#include "optirun/runtime_manager.hpp"
#include <chrono>
#include <future>
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <type_traits>
namespace py = pybind11;
using namespace optirun;
namespace {
Value to_value(py::handle object) {
  if (object.is_none())
    return {};
  if (py::isinstance<py::bool_>(object))
    return object.cast<bool>();
  if (py::isinstance<py::int_>(object))
    return object.cast<std::int64_t>();
  if (py::isinstance<py::float_>(object))
    return object.cast<double>();
  if (py::isinstance<py::str>(object))
    return object.cast<std::string>();
  if (py::isinstance<py::bytes>(object)) {
    const auto bytes = object.cast<std::string>();
    std::vector<std::byte> result;
    result.reserve(bytes.size());
    for (auto byte : bytes)
      result.push_back(
          static_cast<std::byte>(static_cast<unsigned char>(byte)));
    return result;
  }
  throw py::type_error(
      "arguments must be None, bool, int, float, str, or bytes");
}
py::object from_value(const Value &value) {
  return std::visit(
      [](const auto &item) -> py::object {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          return py::none();
        else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
          std::string result;
          result.reserve(item.size());
          for (auto byte : item)
            result.push_back(
                static_cast<char>(std::to_integer<unsigned char>(byte)));
          return py::bytes(result);
        } else
          return py::cast(item);
      },
      value);
}
Backend backend_from(py::handle object) {
  if (py::isinstance<py::str>(object)) {
    const auto name = object.cast<std::string>();
    if (name == "gil")
      return Backend::gil;
    if (name == "free_threaded")
      return Backend::free_threaded;
    throw py::value_error("backend must be 'gil' or 'free_threaded'");
  }
  return object.cast<Backend>();
}
class DispatchFuture {
  std::future<Value> future_;

public:
  explicit DispatchFuture(std::future<Value> future)
      : future_(std::move(future)) {}
  bool done() const {
    return future_.wait_for(std::chrono::seconds(0)) ==
           std::future_status::ready;
  }
  py::object result(py::object timeout = py::none()) {
    try {
      Value value;
      if (timeout.is_none()) {
        {
          py::gil_scoped_release release;
          value = future_.get();
        }
      } else {
        {
          py::gil_scoped_release release;
          if (future_.wait_for(std::chrono::duration<double>(
                  timeout.cast<double>())) == std::future_status::timeout) {
            PyErr_SetString(PyExc_TimeoutError, "dispatch result timed out");
            throw py::error_already_set();
          }
        }
        value = future_.get();
      }
      return from_value(value);
    } catch (const RemoteException &error) {
      py::object exception = py::module_::import("optirun._native")
                                 .attr("RemoteException")(error.what());
      exception.attr("type_name") = error.error().type_name;
      exception.attr("remote_message") = error.error().message;
      exception.attr("traceback") = error.error().traceback;
      PyErr_SetObject(reinterpret_cast<PyObject *>(Py_TYPE(exception.ptr())),
                      exception.ptr());
      throw py::error_already_set();
    }
  }
};
class PyRuntime {
  std::unique_ptr<Runtime> runtime_;
  bool started_{};
  void ensure_started() {
    if (!started_) {
      runtime_->start();
      started_ = true;
    }
  }

public:
  PyRuntime(std::string gil_python, std::string free_python,
            std::string handler_file, std::size_t max_pending,
            double handshake_timeout, double shutdown_timeout) {
    RuntimeConfig config;
    config.gil = {std::move(gil_python), handler_file};
    config.free_threaded = {std::move(free_python), std::move(handler_file)};
    config.max_pending = max_pending;
    config.handshake_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(handshake_timeout));
    config.shutdown_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(shutdown_timeout));
    config.worker_script =
        std::filesystem::path(py::module_::import("optirun._native")
                                  .attr("__file__")
                                  .cast<std::string>())
            .parent_path() /
        "worker.py";
    runtime_ = std::make_unique<Runtime>(std::move(config));
  }
  void register_handler(std::string name, py::iterable backend_values) {
    std::vector<Backend> backends;
    for (auto item : backend_values)
      backends.push_back(backend_from(item));
    runtime_->register_handler(std::move(name), std::move(backends));
  }
  void start() { ensure_started(); }
  DispatchFuture submit(std::string handler, py::iterable argument_values,
                        py::object backend) {
    std::vector<Value> values;
    for (auto item : argument_values)
      values.push_back(to_value(item));
    ensure_started();
    return DispatchFuture(
        runtime_->submit(backend_from(backend), handler, std::move(values)));
  }
  py::dict worker_info(py::object backend) {
    ensure_started();
    const auto info = runtime_->worker_info(backend_from(backend));
    py::dict result;
    result["backend"] = info.backend == Backend::gil ? "gil" : "free_threaded";
    result["executable"] = info.executable.string();
    result["version"] =
        py::make_tuple(info.version[0], info.version[1], info.version[2]);
    result["abiflags"] = info.abiflags;
    result["soabi"] = info.soabi;
    result["build_supports_free_threading"] =
        info.build_supports_free_threading;
    result["gil_enabled"] = info.gil_enabled;
    return result;
  }
  PyRuntime &enter() { return *this; }
  void exit(py::object, py::object, py::object) { shutdown(); }
  void shutdown() {
    if (runtime_)
      runtime_->shutdown();
    started_ = false;
  }
};
} // namespace
PYBIND11_MODULE(_native, m, py::mod_gil_not_used()) {
  py::enum_<Backend>(m, "Backend")
      .value("GIL", Backend::gil)
      .value("FREE_THREADED", Backend::free_threaded);
  py::register_exception<RemoteException>(m, "RemoteException");
  py::class_<DispatchFuture>(m, "DispatchFuture")
      .def("done", &DispatchFuture::done)
      .def("result", &DispatchFuture::result, py::arg("timeout") = py::none());
  py::class_<PyRuntime>(m, "Runtime")
      .def(py::init<std::string, std::string, std::string, std::size_t, double,
                    double>(),
           py::arg("gil_python"), py::arg("free_python"),
           py::arg("handler_file"), py::kw_only(), py::arg("max_pending") = 64,
           py::arg("handshake_timeout") = 5.0,
           py::arg("shutdown_timeout") = 5.0)
      .def("register_handler", &PyRuntime::register_handler)
      .def("start", &PyRuntime::start)
      .def("submit", &PyRuntime::submit, py::arg("handler"),
           py::arg("arguments") = py::make_tuple(), py::kw_only(),
           py::arg("backend"))
      .def("worker_info", &PyRuntime::worker_info)
      .def("shutdown", &PyRuntime::shutdown)
      .def("__enter__", &PyRuntime::enter,
           py::return_value_policy::reference_internal)
      .def("__exit__", &PyRuntime::exit);
}
