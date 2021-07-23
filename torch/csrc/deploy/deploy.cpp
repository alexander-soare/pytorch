#include <c10/util/Exception.h>
#include <torch/csrc/deploy/deploy.h>
#include <torch/cuda.h>

#include <dlfcn.h>
#include <libgen.h>
#include <unistd.h>

char* embedded_interpreter_so_start = nullptr;
char* embedded_interpreter_so_end = nullptr;
char* embedded_interpreter_cuda_so_start = nullptr;
char* embedded_interpreter_cuda_so_end = nullptr;

extern "C" TORCH_API void register_embedded_interpreter(
    char* lib_start,
    char* lib_end) {
  TORCH_CHECK(lib_start != nullptr, "expected non-null lib_start");
  TORCH_CHECK(lib_end != nullptr, "expected non-null lib_end");
  TORCH_CHECK(
      (lib_end - lib_start) > 0, "expected embedded_interpreter_libsize > 0");
  embedded_interpreter_so_start = lib_start;
  embedded_interpreter_so_end = lib_end;
}
// in fbcode, we build the interpreter version with cuda bindings explicitly and
// side-by-side with the one without.  In OSS builds, we just build one
// libinterpreter and it either has or doesn't have cuda depending on top-level
// CMAKE flags
#ifdef FBCODE_CAFFE2
extern "C" TORCH_API void register_embedded_interpreter_cuda(
    char* lib_start,
    char* lib_end) {
  TORCH_CHECK(lib_start != nullptr, "expected non-null lib_start");
  TORCH_CHECK(lib_end != nullptr, "expected non-null lib_end");
  TORCH_CHECK(
      (lib_end - lib_start) > 0,
      "expected embedded_interpreter_cuda_libsize > 0");
  embedded_interpreter_cuda_so_start = lib_start;
  embedded_interpreter_cuda_so_end = lib_end;
}
#else
// TODO(whc) - oss/cmake build needs to be moved over to using
// register_ functions above rather than the symbol below, to also
// support using deploy inside python applications.

// these symbols are generated by cmake, using ld -r -b binary
// libtorch_deployinterpreter.so which takes the contents of the so and embeds
// it into a symbol that is then linked into libtorch_deploy.so. This enables us
// to simply copy the contents of this symbol to disk and dlopen it to create an
// instance of python.
extern "C" __attribute__((
    __weak__)) char _binary_libtorch_deployinterpreter_so_start[];
extern "C"
    __attribute__((__weak__)) char _binary_libtorch_deployinterpreter_so_end[];
#endif

namespace torch {
namespace deploy {

InterpreterManager::InterpreterManager(size_t n_interp) : resources_(n_interp) {
  TORCH_DEPLOY_TRY
  for (const auto i : c10::irange(n_interp)) {
    instances_.emplace_back(this);
    auto I = instances_.back().acquire_session();
    // make torch.version.interp be the interpreter id
    // can be used for balancing work across GPUs
    I.global("torch", "version").attr("__setattr__")({"interp", int(i)});
    // std::cerr << "Interpreter " << i << " initialized\n";
    instances_.back().pImpl_->set_find_module(
        [this](const std::string& name) -> at::optional<std::string> {
          auto it = registered_module_sources_.find(name);
          if (it != registered_module_sources_.end()) {
            return it->second;
          } else {
            return at::nullopt;
          }
        });
  }

  // Pre-registered modules.
  // TODO(jwtan): Make the discovery of these modules easier.
  register_module_source(
      "GetArgumentNamesModule",
      "from inspect import signature\n"
      "def getArgumentNames(function): return list(signature(function).parameters.keys())\n");
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

Package InterpreterManager::load_package(const std::string& uri) {
  TORCH_DEPLOY_TRY
  return Package(uri, this);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

Package InterpreterManager::load_package(
    std::shared_ptr<caffe2::serialize::ReadAdapterInterface> reader) {
  TORCH_DEPLOY_TRY
  return Package(reader, this);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

Obj InterpreterSession::from_movable(const ReplicatedObj& obj) {
  TORCH_DEPLOY_TRY
  return impl_->unpickle_or_get(obj.pImpl_->object_id_, obj.pImpl_->data_);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

InterpreterSession ReplicatedObj::acquire_session(
    const Interpreter* on_this_interpreter) const {
  TORCH_DEPLOY_TRY
  InterpreterSession I = on_this_interpreter
      ? on_this_interpreter->acquire_session()
      : pImpl_->manager_->acquire_one();
  I.self = I.from_movable(*this);
  return I;
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

// NOLINTNEXTLINE(bugprone-exception-escape)
InterpreterSession::~InterpreterSession() {
  if (manager_ && notify_idx_ >= 0) {
    manager_->resources_.free(notify_idx_);
  }
}

void ReplicatedObjImpl::unload(const Interpreter* on_this_interpreter) {
  TORCH_DEPLOY_TRY
  if (!on_this_interpreter) {
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
    for (auto& interp : manager_->all_instances()) {
      unload(&interp);
    }
    return;
  }

  InterpreterSession I = on_this_interpreter->acquire_session();
  I.impl_->unload(object_id_);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

// NOLINTNEXTLINE(bugprone-exception-escape)
ReplicatedObjImpl::~ReplicatedObjImpl() {
  unload(nullptr);
}

void ReplicatedObj::unload(const Interpreter* on_this_interpreter) {
  TORCH_DEPLOY_TRY
  pImpl_->unload(on_this_interpreter);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

ReplicatedObj InterpreterSession::create_movable(Obj obj) {
  TORCH_DEPLOY_TRY
  TORCH_CHECK(
      manager_,
      "Can only create a movable object when the session was created from an interpreter that is part of a InterpreterManager");
  auto pickled = impl_->pickle(self, obj);
  return ReplicatedObj(std::make_shared<ReplicatedObjImpl>(
      manager_->next_object_id_++, std::move(pickled), manager_));
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

void write_tmp_lib(FILE* dst, char* lib_start, char* lib_end) {
  TORCH_INTERNAL_ASSERT(dst);
  size_t size = lib_end - lib_start;
  size_t written = fwrite(lib_start, 1, size, dst);
  TORCH_INTERNAL_ASSERT(size == written, "expected written == size");
}

Interpreter::Interpreter(InterpreterManager* manager)
    : handle_(nullptr), manager_(manager) {
  // NOLINTNEXTLINE(modernize-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
  char library_name[] = "/tmp/torch_deployXXXXXX";
  int fd = mkstemp(library_name);
  TORCH_INTERNAL_ASSERT(fd != -1, "failed to create temporary file");
  library_name_ = library_name;
  FILE* dst = fdopen(fd, "wb");

  // See comment above for fbcode vs oss behavior
  char* lib_start = nullptr;
  char* lib_end = nullptr;
  bool cuda_available = torch::cuda::is_available();
#ifdef FBCODE_CAFFE2
  if (embedded_interpreter_cuda_so_start != nullptr &&
      embedded_interpreter_cuda_so_end != nullptr) {
    lib_start = embedded_interpreter_cuda_so_start;
    lib_end = embedded_interpreter_cuda_so_end;
  } else if (
      embedded_interpreter_so_start != nullptr &&
      embedded_interpreter_so_end != nullptr) {
    lib_start = embedded_interpreter_so_start;
    lib_end = embedded_interpreter_so_end;
  }
#else // FBCODE_CAFFE2
  lib_start = _binary_libtorch_deployinterpreter_so_start;
  lib_end = _binary_libtorch_deployinterpreter_so_end;
#endif // FBCODE_CAFFE2
  std::string cuda_available_str = cuda_available ? "true" : "false";
  TORCH_CHECK(
      lib_start != nullptr && lib_end != nullptr,
      "torch::deploy requires a build-time dependency on embedded_interpreter or embedded_interpreter_cuda, neither of which were found.  torch::cuda::is_available()=" +
          cuda_available_str);

  write_tmp_lib(dst, lib_start, lib_end);
  fclose(dst);
  int flags = RTLD_LOCAL | RTLD_LAZY;
#ifndef FBCODE_CAFFE2
  flags |= RTLD_DEEPBIND;
#endif
  handle_ = dlopen(library_name, flags);
  if (!handle_) {
    throw std::runtime_error(dlerror());
  }

  // note: if you want better debugging symbols for things inside
  // new_intepreter_impl, comment out this line so that the so lasts long enough
  // for the debugger to see it.
  unlink(library_name_.c_str());

#ifndef FBCODE_CAFFE2
  auto deploy_set_self_ptr = (void (*)(void*))dlsym(handle_, "deploy_set_self");
  AT_ASSERT(deploy_set_self_ptr);
  deploy_set_self_ptr(handle_);
#endif

  void* new_interpreter_impl = dlsym(handle_, "new_interpreter_impl");
  AT_ASSERT(new_interpreter_impl);
  pImpl_ = std::unique_ptr<InterpreterImpl>(
      // NOLINTNEXTLINE(modernize-redundant-void-arg)
      ((InterpreterImpl * (*)(void)) new_interpreter_impl)());
}

Interpreter::~Interpreter() {
  if (handle_) {
    // ensure python uninitialization runs before we dlclose the library
    pImpl_.reset();
#ifndef FBCODE_CAFFE2
    auto deploy_flush_python_libs =
        (void (*)(void))dlsym(handle_, "deploy_flush_python_libs");
    deploy_flush_python_libs();
#endif
    dlclose(handle_);
  }
}

int LoadBalancer::acquire() {
  TORCH_DEPLOY_TRY
  thread_local int last = 0;
  size_t minusers = SIZE_MAX;
  int min_idx = 0;
  for (size_t i = 0; i < n_; ++i, ++last) {
    // NOLINTNEXTLINE(clang-diagnostic-sign-compare)
    if (last >= n_) {
      last = 0;
    }
    uint64_t prev = 0;
    bool acquired = __atomic_compare_exchange_n(
        &uses_[8 * last],
        &prev,
        1ULL,
        false,
        __ATOMIC_SEQ_CST,
        __ATOMIC_SEQ_CST);
    if (acquired) {
      // fast path, we found an interpreter with no users
      return last;
    }
    // slow path, we don't want to use this interpreter because it is being
    // used by someone else.

    if (prev < minusers) {
      minusers = prev;
      min_idx = last;
    }
  }
  // we failed to find a completely free interpreter. heuristically use the
  // one with the least number of user (note that this may have changed since
  // then, so this is only a heuristic).
  __atomic_fetch_add(&uses_[8 * min_idx], 1ULL, __ATOMIC_SEQ_CST);
  return min_idx;
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

void LoadBalancer::free(int where) {
  TORCH_DEPLOY_TRY
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  __atomic_fetch_sub(&uses_[8 * where], 1ULL, __ATOMIC_SEQ_CST);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

void PythonMethodWrapper::setArgumentNames(
    std::vector<std::string>& argumentNamesOut) const {
  auto session = model_.acquire_session();
  auto iArgumentNames =
      session
          .global("GetArgumentNamesModule", "getArgumentNames")(
              {session.from_movable(model_)})
          .toIValue();
  TORCH_INTERNAL_ASSERT(iArgumentNames.isList());
  auto argumentNames = iArgumentNames.toListRef();

  argumentNamesOut.reserve(argumentNames.size());
  for (auto& argumentName : argumentNames) {
    TORCH_INTERNAL_ASSERT(argumentName.isString());
    argumentNamesOut.push_back(argumentName.toStringRef());
  }
}

} // namespace deploy
} // namespace torch
