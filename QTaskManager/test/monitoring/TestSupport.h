#pragma once
#include <cmath>
#include <cstdlib>
#include <iostream>
#define CHECK(x)                                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        if (!(x))                                                                                            \
        {                                                                                                    \
            std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #x << '\n';                        \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (false)
#define CHECK_NEAR(a, b, e) CHECK(std::abs((a) - (b)) <= (e))
