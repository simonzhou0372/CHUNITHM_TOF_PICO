#pragma once
constexpr int GPIO_FUNC_I2C=3, GPIO_FUNC_SIO=5, GPIO_IN=0, GPIO_OUT=1;
void gpio_put(unsigned pin, bool value);
bool gpio_get(unsigned pin);
inline void gpio_init(unsigned) {}
inline void gpio_set_function(unsigned, int) {}
inline void gpio_set_dir(unsigned, int) {}
inline void gpio_pull_up(unsigned) {}
