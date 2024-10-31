#ifndef UTIL_STATISTICS_HPP_
#define UTIL_STATISTICS_HPP_

#include <cstdint>
#include <vector>
#include <utility>

// statistics functions implement by Boost.accumulators library

extern uint32_t init_mean_stat();
extern uint32_t init_window_stat(uint32_t window);
extern uint32_t init_histo_stat(uint32_t binN, uint32_t cacheS);
extern uint32_t init_tail_stat(bool dir, uint32_t cacheS);

extern void record_mean_stat(uint32_t handle, double sample);
extern void record_window_stat(uint32_t handle, double sample);
extern void record_histo_stat(uint32_t handle, double sample);
extern void record_tail_stat(uint32_t handle, bool dir, double sample);

extern uint32_t get_mean_count(uint32_t handle);
extern double get_mean_mean(uint32_t handle);
extern double get_mean_error(uint32_t handle);
extern double get_mean_variance(uint32_t handle);
extern uint32_t get_window_count(uint32_t handle);
extern double get_window_mean(uint32_t handle);
extern double get_window_variance(uint32_t handle);
extern uint32_t get_histo_count(uint32_t handle);
extern std::vector<std::pair<double, double> > get_histo_density(uint32_t handle);
extern double get_tail_quantile(uint32_t handle, bool dir, double ratio);

extern void close_mean_stat(uint32_t handle);
extern void close_window_stat(uint32_t handle);
extern void close_histo_stat(uint32_t handle);
extern void close_tail_stat(uint32_t handle, bool dir);

// statistics functions implemented by hands

// return the shape of a distribution (possible to draw a histo graph in one run)
// cdf_points: a list of y-values on the cumulative distribution function (CDF), such as {0.00, 0.25, 0.50, 0.75, 1.00}
// dist: the x-values of the listed CDF points
// sample: the samples.
// Attention: samples are going to sorted during the calculation!
extern void shape_distribution(const std::vector<double>& cdf_points, std::vector<uint64_t> &dist, std::vector<uint64_t> &sample);

// calulate the erlative entropy with a uniform distribution
extern double kl_divergence_with_uniform(const std::vector<uint64_t>& sample);

#endif
