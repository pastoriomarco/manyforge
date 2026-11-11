#include "manyforge_core/pi_mutex.hpp"

#include <memory>
#include <system_error>

namespace manyforge::core {

namespace detail {
void ThrowPiLockError(const int error, const char* const what) {
  throw std::system_error(error, std::generic_category(), what);
}
}  // namespace detail

PiMutex::PiMutex() {
  pthread_mutexattr_t attributes;
  int error = pthread_mutexattr_init(&attributes);
  if (error != 0) detail::ThrowPiLockError(error, "PiMutex attributes");
  error = pthread_mutexattr_setprotocol(&attributes, PTHREAD_PRIO_INHERIT);
  if (error == 0) error = pthread_mutex_init(&mutex_, &attributes);
  (void)pthread_mutexattr_destroy(&attributes);
  if (error != 0) detail::ThrowPiLockError(error, "PiMutex priority inheritance");
}

PiMutex::~PiMutex() { (void)pthread_mutex_destroy(&mutex_); }

PiConditionVariable::PiConditionVariable() : stop_mutex_(std::make_shared<PiMutex>()) {
  pthread_condattr_t attributes;
  int error = pthread_condattr_init(&attributes);
  if (error != 0) detail::ThrowPiLockError(error, "PiConditionVariable attributes");
  error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
  if (error == 0) error = pthread_cond_init(&cond_, &attributes);
  if (error == 0) {
    error = pthread_cond_init(&stop_cond_, &attributes);
    if (error != 0) (void)pthread_cond_destroy(&cond_);
  }
  (void)pthread_condattr_destroy(&attributes);
  if (error != 0) detail::ThrowPiLockError(error, "PiConditionVariable monotonic clock");
}

PiConditionVariable::~PiConditionVariable() {
  (void)pthread_cond_destroy(&stop_cond_);
  (void)pthread_cond_destroy(&cond_);
}

}  // namespace manyforge::core
