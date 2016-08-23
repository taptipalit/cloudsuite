#ifndef RANDNUM_H
#define RANDNUM_H

#include <random>
#include <stdint.h>

typedef uint64_t rand_seed_t;
typedef std::mt19937_64 rand_engine_t;
typedef std::uniform_int_distribution<int64_t> rand_uniform_int_t;
typedef std::uniform_real_distribution<double> rand_uniform_real_t;
typedef std::normal_distribution<double> rand_normal_real_t;

#endif
