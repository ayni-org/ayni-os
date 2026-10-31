#ifndef AYNI_TEST_FREERTOS_H
#define AYNI_TEST_FREERTOS_H
#include <mutex>
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED                                                               \
    {}
#define portENTER_CRITICAL(mutex) (mutex)->lock()
#define portEXIT_CRITICAL(mutex) (mutex)->unlock()
#endif
