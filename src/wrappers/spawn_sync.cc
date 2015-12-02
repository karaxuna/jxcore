// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "spawn_sync.h"
//#include "env-inl.h"
#include "string_bytes.h"
//#include "util.h"

#include <string.h>
#include <stdlib.h>


namespace node {

SyncProcessOutputBuffer::SyncProcessOutputBuffer()
  : used_(0),
  next_(NULL) {
}


void SyncProcessOutputBuffer::OnAlloc(size_t suggested_size,
  uv_buf_t* buf) const {
  if (used() == kBufferSize)
    *buf = uv_buf_init(NULL, 0);
  else
    *buf = uv_buf_init(data_ + used(), available());
}


void SyncProcessOutputBuffer::OnRead(const uv_buf_t* buf, size_t nread) {
  // If we hand out the same chunk twice, this should catch it.
  assert(buf->base == data_ + used());
  used_ += static_cast<unsigned int>(nread);
}


size_t SyncProcessOutputBuffer::Copy(char* dest) const {
  memcpy(dest, data_, used());
  return used();
}


unsigned int SyncProcessOutputBuffer::available() const {
  return sizeof data_ - used();
}


unsigned int SyncProcessOutputBuffer::used() const {
  return used_;
}


SyncProcessOutputBuffer* SyncProcessOutputBuffer::next() const {
  return next_;
}


void SyncProcessOutputBuffer::set_next(SyncProcessOutputBuffer* next) {
  next_ = next;
}


SyncProcessStdioPipe::SyncProcessStdioPipe(SyncProcessRunner* process_handler,
  bool readable,
  bool writable,
  uv_buf_t input_buffer)
  : process_handler_(process_handler),
  readable_(readable),
  writable_(writable),
  input_buffer_(input_buffer),

  first_output_buffer_(NULL),
  last_output_buffer_(NULL),

  uv_pipe_(),
  write_req_(),
  shutdown_req_(),

  lifecycle_(kUninitialized) {
  assert(readable || writable);
}


SyncProcessStdioPipe::~SyncProcessStdioPipe() {
  assert(lifecycle_ == kUninitialized || lifecycle_ == kClosed);

  SyncProcessOutputBuffer* buf;
  SyncProcessOutputBuffer* next;

  for (buf = first_output_buffer_; buf != NULL; buf = next) {
    next = buf->next();
    delete buf;
  }
}


int SyncProcessStdioPipe::Initialize(uv_loop_t* loop) {
  assert(lifecycle_ == kUninitialized);

  int r = uv_pipe_init(loop, uv_pipe(), 0);
  if (r < 0)
    return r;
  
  uv_pipe()->data = this;
  
  lifecycle_ = kInitialized;
  return 0;
}


int SyncProcessStdioPipe::Start() {
  assert(lifecycle_ == kInitialized);

  // Set the busy flag already. If this function fails no recovery is
  // possible.
  lifecycle_ = kStarted;

  if (readable()) {
    if (input_buffer_.len > 0) {
      assert(input_buffer_.base != NULL);

      int r = uv_write(&write_req_,
        uv_stream(),
        &input_buffer_,
        1,
        WriteCallback);
      if (r < 0)
        return r;
    }
    int r = uv_shutdown(&shutdown_req_, uv_stream(), ShutdownCallback);
    if (r < 0)
      return r;
  }

  if (writable()) {
    int r = uv_read_start(uv_stream(), AllocCallback, ReadCallback);
    if (r < 0)
      return r;
  }

  return 0;
}


void SyncProcessStdioPipe::Close() {
  assert(lifecycle_ == kInitialized || lifecycle_ == kStarted);

  uv_close(uv_handle(), CloseCallback);

  lifecycle_ = kClosing;
}


JS_LOCAL_OBJECT SyncProcessStdioPipe::GetOutputAsBuffer() const {
  size_t length = OutputLength();
  Buffer *js_buffer = Buffer::New(length);
  CopyOutput(Buffer::Data(js_buffer));
  return JS_OBJECT_FROM_PERSISTENT(js_buffer->handle_);
}


bool SyncProcessStdioPipe::readable() const {
  return readable_;
}


bool SyncProcessStdioPipe::writable() const {
  return writable_;
}


uv_stdio_flags SyncProcessStdioPipe::uv_flags() const {
  unsigned int flags;

  flags = UV_CREATE_PIPE;
  if (readable())
    flags |= UV_READABLE_PIPE;
  if (writable())
    flags |= UV_WRITABLE_PIPE;

  return static_cast<uv_stdio_flags>(flags);
}


uv_pipe_t* SyncProcessStdioPipe::uv_pipe() const {
  assert(lifecycle_ < kClosing);
  return &uv_pipe_;
}


uv_stream_t* SyncProcessStdioPipe::uv_stream() const {
  return reinterpret_cast<uv_stream_t*>(uv_pipe());
}


uv_handle_t* SyncProcessStdioPipe::uv_handle() const {
  return reinterpret_cast<uv_handle_t*>(uv_pipe());
}


size_t SyncProcessStdioPipe::OutputLength() const {
  SyncProcessOutputBuffer* buf;
  size_t size = 0;

  for (buf = first_output_buffer_; buf != NULL; buf = buf->next())
    size += buf->used();

  return size;
}


void SyncProcessStdioPipe::CopyOutput(char* dest) const {
  SyncProcessOutputBuffer* buf;
  size_t offset = 0;

  for (buf = first_output_buffer_; buf != NULL; buf = buf->next())
    offset += buf->Copy(dest + offset);
}


void SyncProcessStdioPipe::OnAlloc(size_t suggested_size, uv_buf_t* buf) {
  // This function assumes that libuv will never allocate two buffers for the
  // same stream at the same time. There's an assert in
  // SyncProcessOutputBuffer::OnRead that would fail if this assumption was
  // ever violated.

  if (last_output_buffer_ == NULL) {
    // Allocate the first capture buffer.
    first_output_buffer_ = new SyncProcessOutputBuffer();
    last_output_buffer_ = first_output_buffer_;

  }
  else if (last_output_buffer_->available() == 0) {
    // The current capture buffer is full so get us a new one.
    SyncProcessOutputBuffer* buf = new SyncProcessOutputBuffer();
    last_output_buffer_->set_next(buf);
    last_output_buffer_ = buf;
  }

  last_output_buffer_->OnAlloc(suggested_size, buf);
}


void SyncProcessStdioPipe::OnRead(const uv_buf_t* buf, ssize_t nread) {
  if (nread == UV_EOF) {
    // Libuv implicitly stops reading on EOF.

  }
  else if (nread < 0) {
    SetError(static_cast<int>(nread));
    // At some point libuv should really implicitly stop reading on error.
    uv_read_stop(uv_stream());

  }
  else {
    last_output_buffer_->OnRead(buf, nread);
    process_handler_->IncrementBufferSizeAndCheckOverflow(nread);
  }
}


void SyncProcessStdioPipe::OnWriteDone(int result) {
  if (result < 0)
    SetError(result);
}


void SyncProcessStdioPipe::OnShutdownDone(int result) {
  if (result < 0)
    SetError(result);
}


void SyncProcessStdioPipe::OnClose() {
  lifecycle_ = kClosed;
}


void SyncProcessStdioPipe::SetError(int error) {
  assert(error != 0);
  process_handler_->SetPipeError(error);
}

// not sure about this!
uv_buf_t SyncProcessStdioPipe::AllocCallback(uv_handle_t* handle, size_t suggested_size) {
  JS_ENTER_SCOPE_COM();
  JS_HANDLE_OBJECT objl = JS_HANDLE_OBJECT();
  char* buf = com->s_slab_allocator->Allocate(objl, suggested_size);

  SyncProcessStdioPipe* self =
    reinterpret_cast<SyncProcessStdioPipe*>(handle->data);
  return uv_buf_init(buf, suggested_size);
}


void SyncProcessStdioPipe::ReadCallback(uv_stream_t* stream,
  ssize_t nread, uv_buf_t buf) {
  SyncProcessStdioPipe* self =
    reinterpret_cast<SyncProcessStdioPipe*>(stream->data);

  self->OnRead(&buf, nread);
}


void SyncProcessStdioPipe::WriteCallback(uv_write_t* req, int result) {
  SyncProcessStdioPipe* self =
    reinterpret_cast<SyncProcessStdioPipe*>(req->handle->data);
  self->OnWriteDone(result);
}


void SyncProcessStdioPipe::ShutdownCallback(uv_shutdown_t* req, int result) {
  SyncProcessStdioPipe* self =
    reinterpret_cast<SyncProcessStdioPipe*>(req->handle->data);

  // On AIX, OS X and the BSDs, calling shutdown() on one end of a pipe
  // when the other end has closed the connection fails with ENOTCONN.
  // Libuv is not the right place to handle that because it can't tell
  // if the error is genuine but we here can.
  if (result == UV_ENOTCONN)
    result = 0;

  self->OnShutdownDone(result);
}


void SyncProcessStdioPipe::CloseCallback(uv_handle_t* handle) {
  SyncProcessStdioPipe* self =
    reinterpret_cast<SyncProcessStdioPipe*>(handle->data);
  self->OnClose();
}


JS_METHOD(SyncProcessRunner, Spawn) {
  SyncProcessRunner p = SyncProcessRunner();
  JS_LOCAL_VALUE result = p.Run(JS_VALUE_TO_OBJECT(args.GetItem(0)), args);
  RETURN_PARAM(result);
}
JS_METHOD_END


DECLARE_CLASS_INITIALIZER(SyncProcessRunner::Initialize) {
  JS_ENTER_SCOPE_COM();
  JS_DEFINE_STATE_MARKER(com);
  JS_METHOD_SET(target, "spawn", SyncProcessRunner::Spawn);
}


SyncProcessRunner::SyncProcessRunner()
  : max_buffer_(0),
  timeout_(0),
  kill_signal_(SIGTERM),

  uv_loop_(NULL),

  stdio_count_(0),
  uv_stdio_containers_(NULL),
  stdio_pipes_(NULL),
  stdio_pipes_initialized_(false),

  uv_process_options_(),
  file_buffer_(NULL),
  args_buffer_(NULL),
  env_buffer_(NULL),
  cwd_buffer_(NULL),

  uv_process_(),
  killed_(false),

  buffered_output_size_(0),
  exit_status_(-1),
  term_signal_(-1),

  uv_timer_(),
  kill_timer_initialized_(false),

  error_(0),
  pipe_error_(0),

  lifecycle_(kUninitialized) {
}


SyncProcessRunner::~SyncProcessRunner() {
  assert(lifecycle_ == kHandlesClosed);

  if (stdio_pipes_ != NULL) {
    for (size_t i = 0; i < stdio_count_; i++) {
      if (stdio_pipes_[i] != NULL)
        delete stdio_pipes_[i];
    }
  }

  delete[] stdio_pipes_;
  delete[] file_buffer_;
  delete[] args_buffer_;
  delete[] cwd_buffer_;
  delete[] env_buffer_;
  delete[] uv_stdio_containers_;
}


JS_LOCAL_OBJECT SyncProcessRunner::Run(JS_LOCAL_VALUE options, jxcore::PArguments args) {
  assert(lifecycle_ == kUninitialized);

  TryInitializeAndRunLoop(options);
  CloseHandlesAndDeleteLoop();

  JS_LOCAL_OBJECT result = BuildResultObject(args);
  return result;
}


void SyncProcessRunner::TryInitializeAndRunLoop(JS_LOCAL_VALUE options) {
  int r;

  // There is no recovery from failure inside TryInitializeAndRunLoop - the
  // only option we'd have is to close all handles and destroy the loop.
  assert(lifecycle_ == kUninitialized);
  lifecycle_ = kInitialized;

  uv_loop_ = new uv_loop_t;
  
  if (uv_loop_ == NULL)
    return SetError(UV_ENOMEM);
  assert(uv_loop_init(uv_loop_) == 0);

  r = ParseOptions(options);
  if (r < 0)
    return SetError(r);
  
  if (timeout_ > 0) {
    r = uv_timer_init(uv_loop_, &uv_timer_);
    if (r < 0)
      return SetError(r);
  
    uv_unref(reinterpret_cast<uv_handle_t*>(&uv_timer_));
    
    uv_timer_.data = this;
    kill_timer_initialized_ = true;
    
    // Start the timer immediately. If uv_spawn fails then
    // CloseHandlesAndDeleteLoop() will immediately close the timer handle
    // which implicitly stops it, so there is no risk that the timeout callback
    // runs when the process didn't start.
    r = uv_timer_start(&uv_timer_, KillTimerCallback, timeout_, 0);
    if (r < 0)
      return SetError(r);
  }

  uv_process_options_.exit_cb = ExitCallback;
  r = uv_spawn(uv_loop_, &uv_process_, uv_process_options_);
  if (r < 0)
    return SetError(r);
  uv_process_.data = this;
  
  for (uint32_t i = 0; i < stdio_count_; i++) {
    SyncProcessStdioPipe* h = stdio_pipes_[i];
    if (h != NULL) {
      r = h->Start();
      if (r < 0)
        return SetPipeError(r);
    }
  }
  
  r = uv_run(uv_loop_, UV_RUN_DEFAULT);
  if (r < 0)
    // We can't handle uv_run failure.
    abort();
  
  // If we get here the process should have exited.
  assert(exit_status_ >= 0);
}


void SyncProcessRunner::CloseHandlesAndDeleteLoop() {
  assert(lifecycle_ < kHandlesClosed);

  if (uv_loop_ != NULL) {
    CloseStdioPipes();
    CloseKillTimer();
    // Close the process handle when ExitCallback was not called.
    uv_handle_t* uv_process_handle =
      reinterpret_cast<uv_handle_t*>(&uv_process_);
    if (!uv_is_closing(uv_process_handle))
      uv_close(uv_process_handle, NULL);

    // Give closing watchers a chance to finish closing and get their close
    // callbacks called.
    int r = uv_run(uv_loop_, UV_RUN_DEFAULT);
    if (r < 0)
      abort();

    assert(uv_loop_close(uv_loop_) == 0);
    delete uv_loop_;
    uv_loop_ = NULL;

  }
  else {
    // If the loop doesn't exist, neither should any pipes or timers.
    assert(!stdio_pipes_initialized_);
    assert(!kill_timer_initialized_);
  }

  lifecycle_ = kHandlesClosed;
}


void SyncProcessRunner::CloseStdioPipes() {
  assert(lifecycle_ < kHandlesClosed);

  if (stdio_pipes_initialized_) {
    assert(stdio_pipes_ != NULL);
    assert(uv_loop_ != NULL);

    for (uint32_t i = 0; i < stdio_count_; i++) {
      if (stdio_pipes_[i] != NULL)
        stdio_pipes_[i]->Close();
    }

    stdio_pipes_initialized_ = false;
  }
}


void SyncProcessRunner::CloseKillTimer() {
  assert(lifecycle_ < kHandlesClosed);

  if (kill_timer_initialized_) {
    assert(timeout_ > 0);
    assert(uv_loop_ != NULL);

    uv_handle_t* uv_timer_handle = reinterpret_cast<uv_handle_t*>(&uv_timer_);
    uv_ref(uv_timer_handle);
    uv_close(uv_timer_handle, KillTimerCloseCallback);

    kill_timer_initialized_ = false;
  }
}


void SyncProcessRunner::Kill() {
  // Only attempt to kill once.
  if (killed_)
    return;
  killed_ = true;

  // We might get here even if the process we spawned has already exited. This
  // could happen when our child process spawned another process which
  // inherited (one of) the stdio pipes. In this case we won't attempt to send
  // a signal to the process, however we will still close our end of the stdio
  // pipes so this situation won't make us hang.
  if (exit_status_ < 0) {
    int r = uv_process_kill(&uv_process_, kill_signal_);

    // If uv_kill failed with an error that isn't ESRCH, the user probably
    // specified an invalid or unsupported signal. Signal this to the user as
    // and error and kill the process with SIGKILL instead.
    if (r < 0 && r != UV_ESRCH) {
      SetError(r);

      r = uv_process_kill(&uv_process_, SIGKILL);
      assert(r >= 0 || r == UV_ESRCH);
    }
  }

  // Close all stdio pipes.
  CloseStdioPipes();

  // Stop the timeout timer immediately.
  CloseKillTimer();
}


void SyncProcessRunner::IncrementBufferSizeAndCheckOverflow(ssize_t length) {
  buffered_output_size_ += length;

  if (max_buffer_ > 0 && buffered_output_size_ > max_buffer_) {
    SetError(UV_ENOBUFS);
    Kill();
  }
}


void SyncProcessRunner::OnExit(int64_t exit_status, int term_signal) {
  if (exit_status < 0)
    return SetError(static_cast<int>(exit_status));

  exit_status_ = exit_status;
  term_signal_ = term_signal;
}


void SyncProcessRunner::OnKillTimerTimeout() {
  SetError(UV_ETIMEDOUT);
  Kill();
}


int SyncProcessRunner::GetError() {
  if (error_ != 0)
    return error_;
  else
    return pipe_error_;
}


void SyncProcessRunner::SetError(int error) {
  if (error_ == 0)
    error_ = error;
}


void SyncProcessRunner::SetPipeError(int pipe_error) {
  if (pipe_error_ == 0)
    pipe_error_ = pipe_error;
}


JS_LOCAL_ARRAY SyncProcessRunner::BuildResultObject(jxcore::PArguments args) {
  JS_ENTER_SCOPE_COM();
  JS_DEFINE_STATE_MARKER(com);

  JS_LOCAL_OBJECT js_result = JS_NEW_EMPTY_OBJECT();
  if (GetError() != 0) {
    JS_NAME_SET(js_result, JS_STRING_ID("error"),
      STD_TO_INTEGER(GetError()));
  }

  if (exit_status_ >= 0)
    JS_NAME_SET(js_result, JS_STRING_ID("status"),
      STD_TO_INTEGER(static_cast<double>(exit_status_)));
  else
    // If exit_status_ < 0 the process was never started because of some error.
    JS_NAME_SET(js_result, JS_STRING_ID("status"),
      JS_NULL());

  if (term_signal_ > 0)
    JS_NAME_SET(js_result, JS_STRING_ID("signal"),
      STD_TO_STRING(signo_string(term_signal_)));
  else
    JS_NAME_SET(js_result, JS_STRING_ID("signal"),
      JS_NULL());

  if (exit_status_ >= 0)
    JS_NAME_SET(js_result, JS_STRING_ID("output"),
      BuildOutputArray(args));
  else
    JS_NAME_SET(js_result, JS_STRING_ID("output"),
      JS_NULL());

  JS_NAME_SET(js_result, JS_STRING_ID("pid"),
    STD_TO_INTEGER(uv_process_.pid));

  return JS_TYPE_AS_ARRAY(js_result);
}


JS_LOCAL_ARRAY SyncProcessRunner::BuildOutputArray(jxcore::PArguments args) {
  assert(lifecycle_ >= kInitialized);
  assert(stdio_pipes_ != NULL);

  JS_ENTER_SCOPE_COM();
  JS_DEFINE_STATE_MARKER(com);
  
  JS_LOCAL_ARRAY js_output = JS_NEW_ARRAY_WITH_COUNT(stdio_count_);

  for (uint32_t i = 0; i < stdio_count_; i++) {
    SyncProcessStdioPipe* h = stdio_pipes_[i];
    if (h != NULL && h->writable())
      JS_INDEX_SET(js_output, i, h->GetOutputAsBuffer());
    else
      JS_INDEX_SET(js_output, i, JS_NULL());
  }
  return js_output;
}


int SyncProcessRunner::ParseOptions(JS_LOCAL_VALUE js_value) {
  JS_ENTER_SCOPE_COM();
  JS_DEFINE_STATE_MARKER(com);

  int r;

  if (!js_value->IsObject())
    return UV_EINVAL;
  
  JS_LOCAL_OBJECT js_options = JS_VALUE_TO_OBJECT(js_value);

  JS_LOCAL_VALUE js_file = JS_GET_NAME(js_options, STD_TO_STRING("file"));
  r = CopyJsString(js_file, &file_buffer_);
  if (r < 0)
    return r;
  uv_process_options_.file = file_buffer_;
  
  JS_LOCAL_VALUE js_args = JS_GET_NAME(js_options, STD_TO_STRING("args"));
  r = CopyJsStringArray(js_args, &args_buffer_);
  if (r < 0)
    return r;
  uv_process_options_.args = reinterpret_cast<char**>(args_buffer_);
  
  
  JS_LOCAL_VALUE js_cwd = JS_GET_NAME(js_options, STD_TO_STRING("cwd"));
  if (IsSet(js_cwd)) {
    r = CopyJsString(js_cwd, &cwd_buffer_);
    if (r < 0)
      return r;
    uv_process_options_.cwd = (char*)cwd_buffer_;
  }
  
  JS_LOCAL_VALUE js_env_pairs = JS_GET_NAME(js_options, STD_TO_STRING("envPairs"));
  if (IsSet(js_env_pairs)) {
    r = CopyJsStringArray(js_env_pairs, &env_buffer_);
    if (r < 0)
      return r;
  
    uv_process_options_.env = reinterpret_cast<char**>(env_buffer_);
  }
  JS_LOCAL_VALUE js_uid = JS_GET_NAME(js_options, STD_TO_STRING("uid"));
  if (IsSet(js_uid)) {
    if (!CheckRange<uv_uid_t>(js_uid))
      return UV_EINVAL;
    uv_process_options_.uid = static_cast<uv_gid_t>(js_uid->Int32Value());
    uv_process_options_.flags |= UV_PROCESS_SETUID;
  }
  
  JS_LOCAL_VALUE js_gid = JS_GET_NAME(js_options, STD_TO_STRING("gid"));
  if (IsSet(js_gid)) {
    if (!CheckRange<uv_gid_t>(js_gid))
      return UV_EINVAL;
    uv_process_options_.gid = static_cast<uv_gid_t>(js_gid->Int32Value());
    uv_process_options_.flags |= UV_PROCESS_SETGID;
  }
  
  if (JS_GET_NAME(js_options, STD_TO_STRING("detached"))->BooleanValue())
    uv_process_options_.flags |= UV_PROCESS_DETACHED;
  
  JS_LOCAL_STRING wba = STD_TO_STRING("windowsVerbatimArguments");
  if (JS_GET_NAME(js_options, wba)->BooleanValue())
    uv_process_options_.flags |= UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS;
  
  JS_LOCAL_VALUE js_timeout = JS_GET_NAME(js_options, STD_TO_STRING("timeout"));
  if (IsSet(js_timeout)) {
    if (!js_timeout->IsNumber())
      return UV_EINVAL;
    int64_t timeout = js_timeout->IntegerValue();
    if (timeout < 0)
      return UV_EINVAL;
    timeout_ = static_cast<uint64_t>(timeout);
  }
  
  JS_LOCAL_VALUE js_max_buffer = JS_GET_NAME(js_options, STD_TO_STRING("maxBuffer"));
  if (IsSet(js_max_buffer)) {
    if (!CheckRange<uint32_t>(js_max_buffer))
      return UV_EINVAL;
    max_buffer_ = js_max_buffer->Uint32Value();
  }
  
  JS_LOCAL_VALUE js_kill_signal = JS_GET_NAME(js_options, STD_TO_STRING("killSignal"));
  if (IsSet(js_kill_signal)) {
    if (!JS_IS_INT32(js_kill_signal))
      return UV_EINVAL;
    kill_signal_ = js_kill_signal->Int32Value();
    if (kill_signal_ == 0)
      return UV_EINVAL;
  }
  
  JS_LOCAL_VALUE js_stdio = JS_GET_NAME(js_options, STD_TO_STRING("stdio"));
  r = ParseStdioOptions(js_stdio);
  if (r < 0)
    return r;

  return 0;
}


int SyncProcessRunner::ParseStdioOptions(JS_LOCAL_VALUE js_value) {
  JS_ENTER_SCOPE_COM();
  JS_DEFINE_STATE_MARKER(com);

  JS_LOCAL_ARRAY js_stdio_options;

  if (!JS_IS_ARRAY(js_value))
    return UV_EINVAL;

  js_stdio_options = JS_TYPE_AS_ARRAY(js_value);

  stdio_count_ = JS_GET_ARRAY_LENGTH(js_stdio_options);
  uv_stdio_containers_ = new uv_stdio_container_t[stdio_count_];

  stdio_pipes_ = new SyncProcessStdioPipe*[stdio_count_]();
  stdio_pipes_initialized_ = true;

  for (uint32_t i = 0; i < stdio_count_; i++) {
    JS_LOCAL_VALUE js_stdio_option = JS_GET_INDEX(js_stdio_options, i);

    if (!JS_IS_OBJECT(js_stdio_option))
      return UV_EINVAL;

    int r = ParseStdioOption(i, JS_CAST_OBJECT(js_stdio_option));
    if (r < 0)
      return r;
  }

  uv_process_options_.stdio = uv_stdio_containers_;
  uv_process_options_.stdio_count = stdio_count_;

  return 0;
}


int SyncProcessRunner::ParseStdioOption(int child_fd,
  JS_LOCAL_OBJECT js_stdio_option) {
  JS_ENTER_SCOPE_COM();
  JS_DEFINE_STATE_MARKER(com);

  JS_LOCAL_VALUE js_type = JS_GET_NAME(js_stdio_option, STD_TO_STRING("type"));

  if (js_type->StrictEquals(STD_TO_STRING("ignore"))) {
    return AddStdioIgnore(child_fd);

  }
  else if (js_type->StrictEquals(STD_TO_STRING("pipe"))) {
    JS_LOCAL_STRING rs = STD_TO_STRING("readable");
    JS_LOCAL_STRING ws = STD_TO_STRING("writable");

    bool readable = JS_GET_NAME(js_stdio_option, rs)->BooleanValue();
    bool writable = JS_GET_NAME(js_stdio_option, ws)->BooleanValue();

    uv_buf_t buf = uv_buf_init(NULL, 0);
    
    if (readable) {
      JS_LOCAL_VALUE input = JS_GET_NAME(js_stdio_option, STD_TO_STRING("input"));
      if (Buffer::HasInstance(input)) {
        buf = uv_buf_init(Buffer::Data(input),
          static_cast<unsigned int>(Buffer::Length(input)));
      }
      else if (!input->IsUndefined() && !input->IsNull()) {
        // Strings, numbers etc. are currently unsupported. It's not possible
        // to create a buffer for them here because there is no way to free
        // them afterwards.
        return UV_EINVAL;
      }
    }
    
    return AddStdioPipe(child_fd, readable, writable, buf);

  }
  else if (js_type->StrictEquals(STD_TO_STRING("inherit")) ||
    js_type->StrictEquals(STD_TO_STRING("fd"))) {
    int inherit_fd = JS_GET_NAME(js_stdio_option, STD_TO_STRING("fd"))->Int32Value();
    return AddStdioInheritFD(child_fd, inherit_fd);

  }
  else {
    assert(0 && "invalid child stdio type");
    return UV_EINVAL;
  }
}


int SyncProcessRunner::AddStdioIgnore(uint32_t child_fd) {
  assert(child_fd < stdio_count_);
  assert(stdio_pipes_[child_fd] == NULL);

  uv_stdio_containers_[child_fd].flags = UV_IGNORE;

  return 0;
}


int SyncProcessRunner::AddStdioPipe(uint32_t child_fd,
  bool readable,
  bool writable,
  uv_buf_t input_buffer) {
  assert(child_fd < stdio_count_);
  assert(stdio_pipes_[child_fd] == NULL);

  SyncProcessStdioPipe* h = new SyncProcessStdioPipe(this,
    readable,
    writable,
    input_buffer);

  int r = h->Initialize(uv_loop_);
  if (r < 0) {
    delete h;
    return r;
  }
  
  stdio_pipes_[child_fd] = h;
  
  uv_stdio_containers_[child_fd].flags = h->uv_flags();
  uv_stdio_containers_[child_fd].data.stream = h->uv_stream();

  return 0;
}


int SyncProcessRunner::AddStdioInheritFD(uint32_t child_fd, int inherit_fd) {
  assert(child_fd < stdio_count_);
  assert(stdio_pipes_[child_fd] == NULL);

  uv_stdio_containers_[child_fd].flags = UV_INHERIT_FD;
  uv_stdio_containers_[child_fd].data.fd = inherit_fd;

  return 0;
}


bool SyncProcessRunner::IsSet(JS_LOCAL_VALUE value) {
  return !value->IsUndefined() && !value->IsNull();
}


template <typename t>
bool SyncProcessRunner::CheckRange(JS_LOCAL_VALUE js_value) {
  if ((t)-1 > 0) {
    // Unsigned range check.
    if (!js_value->IsUint32())
      return false;
    if (js_value->Uint32Value() & ~((t)~0))
      return false;

  }
  else {
    // Signed range check.
    if (!js_value->IsInt32())
      return false;
    if (js_value->Int32Value() & ~((t)~0))
      return false;
  }

  return true;
}


int SyncProcessRunner::CopyJsString(JS_LOCAL_VALUE js_value,
  const char** target) {
  JS_ENTER_SCOPE_COM();
  JS_DEFINE_STATE_MARKER(com);

  JS_LOCAL_STRING js_string;
  size_t size, written;
  char* buffer;

  // ToDo: how to cast MozJS::Value to MozJS::String?
  //if (js_value->IsString())
  //  js_string = JS_TYPE_AS_STRING(js_value);
  //else
    js_string = JS_VALUE_TO_STRING(js_value);

  // Include space for null terminator byte.
  
  size = StringBytes::StorageSize(js_string, UTF8) + 1;

  buffer = new char[size];

  written = StringBytes::Write(buffer, -1, js_string, UTF8);
  buffer[written] = '\0';

  *target = buffer;
  return 0;
}

int SyncProcessRunner::CopyJsStringArray(JS_LOCAL_VALUE js_value,
  char** target) {
  JS_ENTER_SCOPE_COM();
  JS_DEFINE_STATE_MARKER(com);

  JS_LOCAL_ARRAY js_array;
  uint32_t length;
  size_t list_size, data_size, data_offset;
  char** list;
  char* buffer;

  if (!JS_IS_ARRAY(js_value))
    return UV_EINVAL;
  
  JS_LOCAL_ARRAY js_value_array = JS_TYPE_AS_ARRAY(js_value);
  length = JS_GET_ARRAY_LENGTH(js_value_array);
  js_array = JS_NEW_ARRAY_WITH_COUNT(length);

  // Convert all array elements to string. Modify the js object itself if
  // needed - it's okay since we cloned the original object.
  for (uint32_t i = 0; i < length; i++) {
    if (!JS_IS_STRING(JS_GET_INDEX(js_value_array, i)))
      JS_INDEX_SET(js_array, i, JS_VALUE_TO_STRING(JS_GET_INDEX(js_value_array, i)));
  }

  // Index has a pointer to every string element, plus one more for a final
  // null pointer.
  list_size = (length + 1) * sizeof *list;

  // Compute the length of all strings. Include room for null terminator
  // after every string. Align strings to cache lines.
  data_size = 0;
  for (uint32_t i = 0; i < length; i++) {
    data_size += StringBytes::StorageSize(JS_GET_INDEX(js_array, i), UTF8) + 1;
    data_size = ROUND_UP(data_size, sizeof(void*));  // NOLINT(runtime/sizeof)
  }

  buffer = new char[list_size + data_size];

  list = reinterpret_cast<char**>(buffer);
  data_offset = list_size;

  for (uint32_t i = 0; i < length; i++) {
    list[i] = buffer + data_offset;
    data_offset += StringBytes::Write(
      buffer + data_offset,
      -1,
      JS_GET_INDEX(js_array, i),
      UTF8);
    buffer[data_offset++] = '\0';
    data_offset = ROUND_UP(data_offset,
      sizeof(void*));  // NOLINT(runtime/sizeof)
  }

  list[length] = NULL;

  *target = buffer;
  return 0;
}


void SyncProcessRunner::ExitCallback(uv_process_t* handle,
  int exit_status,
  int term_signal) {
  SyncProcessRunner* self = reinterpret_cast<SyncProcessRunner*>(handle->data);
  uv_close(reinterpret_cast<uv_handle_t*>(handle), NULL);
  self->OnExit(exit_status, term_signal);
}


void SyncProcessRunner::KillTimerCallback(uv_timer_t* handle, int status) {
  SyncProcessRunner* self = reinterpret_cast<SyncProcessRunner*>(handle->data);
  self->OnKillTimerTimeout();
}


void SyncProcessRunner::KillTimerCloseCallback(uv_handle_t* handle) {
  // No-op.
}

}  // namespace node

NODE_MODULE(node_spawn_sync, node::SyncProcessRunner::Initialize)
