#include "util/statistics.hpp"

#include <map>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/density.hpp>
#include <boost/accumulators/statistics/tail_quantile.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/rolling_mean.hpp>
#include <boost/accumulators/statistics/error_of.hpp>
#include <boost/accumulators/statistics/error_of_mean.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/accumulators/statistics/rolling_variance.hpp>
#include <cassert>
#include <algorithm>
#include <numeric>
#include <cmath>

using namespace boost::accumulators;

static uint32_t handles = 0;
static std::map<uint32_t, void *> db;

typedef accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean>, tag::variance(lazy) > > stat_mean_t;
typedef accumulator_set<double, stats<tag::rolling_mean, tag::rolling_variance(lazy) > > stat_window_t;

typedef accumulator_set<double, stats<tag::density > > stat_histo_t;
typedef boost::iterator_range<std::vector<std::pair<double, double> >::iterator > stat_histo_iter_t;

typedef accumulator_set<double, stats<tag::tail_quantile<left> > > stat_tail_left_t;
typedef accumulator_set<double, stats<tag::tail_quantile<right> > > stat_tail_right_t;

uint32_t init_mean_stat() {
  db[handles] = (void *)(new stat_mean_t());
  return handles++;
}

uint32_t init_window_stat(uint32_t window) {
  db[handles] = (void *)(new stat_window_t(tag::rolling_window::window_size = window));
  return handles++;
}

uint32_t init_histo_stat(uint32_t binN, uint32_t cacheS) {
  db[handles] = (void *)(new stat_histo_t(tag::density::num_bins = binN,
                                             tag::density::cache_size = cacheS));
  return handles++;
}

uint32_t init_tail_stat(bool dir, uint32_t cacheS) {
  if(dir)
    db[handles] = (void *)(new stat_tail_left_t(tag::tail<left>::cache_size = cacheS));
  else
    db[handles] = (void *)(new stat_tail_right_t(tag::tail<right>::cache_size = cacheS));
  return handles++;
}

void record_mean_stat(uint32_t handle, double sample) {
  assert(db.count(handle));
  stat_mean_t *stat = (stat_mean_t *)(db[handle]);
  (*stat)(sample);
}

void record_window_stat(uint32_t handle, double sample) {
  assert(db.count(handle));
  stat_window_t *stat = (stat_window_t *)(db[handle]);
  (*stat)(sample);
}

void record_histo_stat(uint32_t handle, double sample) {
  assert(db.count(handle));
  stat_histo_t *stat = (stat_histo_t *)(db[handle]);
  (*stat)(sample);
}

void record_tail_stat(uint32_t handle, bool dir, double sample) {
  assert(db.count(handle));
  if(dir) {
    stat_tail_right_t *stat = (stat_tail_right_t *)(db[handle]);
    (*stat)(sample);
  } else {
    stat_tail_left_t *stat = (stat_tail_left_t *)(db[handle]);
    (*stat)(sample);
  }
}

uint32_t get_mean_count(uint32_t handle) {
  assert(db.count(handle));
  stat_mean_t *stat = (stat_mean_t *)(db[handle]);
  return count(*stat);
}

double get_mean_mean(uint32_t handle) {
  assert(db.count(handle));
  stat_mean_t *stat = (stat_mean_t *)(db[handle]);
  return mean(*stat);
}

double get_mean_error(uint32_t handle) {
  assert(db.count(handle));
  stat_mean_t *stat = (stat_mean_t *)(db[handle]);
  return error_of<tag::mean>(*stat);
}

double get_mean_variance(uint32_t handle) {
  assert(db.count(handle));
  stat_mean_t *stat = (stat_mean_t *)(db[handle]);
  return variance(*stat);
}

uint32_t get_window_count(uint32_t handle) {
  assert(db.count(handle));
  stat_window_t *stat = (stat_window_t *)(db[handle]);
  return rolling_count(*stat);
}

double get_window_mean(uint32_t handle) {
  assert(db.count(handle));
  stat_window_t *stat = (stat_window_t *)(db[handle]);
  return rolling_mean(*stat);
}

double get_window_variance(uint32_t handle) {
  assert(db.count(handle));
  stat_window_t *stat = (stat_window_t *)(db[handle]);
  return rolling_variance(*stat);
}

uint32_t get_histo_count(uint32_t handle) {
  assert(db.count(handle));
  stat_histo_t *stat = (stat_histo_t *)(db[handle]);
  return count(*stat);
}

std::vector<std::pair<double, double> > get_histo_density(uint32_t handle) {
  assert(db.count(handle));
  stat_histo_t *stat = (stat_histo_t *)(db[handle]);
  stat_histo_iter_t hist = density(*stat);
  std::vector<std::pair<double, double> > rv(hist.size());
  for(uint32_t i=0; i<hist.size(); i++) rv[i] = hist[i];
  return rv;
}

double get_tail_quantile(uint32_t handle, bool dir, double ratio) {
  assert(db.count(handle));
  if(dir) {
    stat_tail_right_t *stat = (stat_tail_right_t *)(db[handle]);
    assert(ratio >= 0.5);
    return quantile(*stat, quantile_probability = ratio);
  } else {
    stat_tail_left_t *stat = (stat_tail_left_t *)(db[handle]);
    assert(ratio <= 0.5);
    return quantile(*stat, quantile_probability = ratio);
  }
}

void close_mean_stat(uint32_t handle) {
  assert(db.count(handle));
  stat_mean_t *stat = (stat_mean_t *)(db[handle]);
  delete stat;
  db.erase(handle);
}

void close_window_stat(uint32_t handle) {
  assert(db.count(handle));
  stat_window_t *stat = (stat_window_t *)(db[handle]);
  delete stat;
  db.erase(handle);
}

void close_histo_stat(uint32_t handle) {
  assert(db.count(handle));
  stat_histo_t *stat = (stat_histo_t *)(db[handle]);
  delete stat;
  db.erase(handle);
}

void close_tail_stat(uint32_t handle, bool dir) {
  assert(db.count(handle));
  if(dir) {
    stat_tail_right_t *stat = (stat_tail_right_t *)(db[handle]);
    delete stat;
  } else {
    stat_tail_left_t *stat = (stat_tail_left_t *)(db[handle]);
    delete stat;
  }
  db.erase(handle);
}

void shape_distribution(const std::vector<double>& cdfs, std::vector<uint64_t> &dist, std::vector<uint64_t> &sample) {
  assert(dist.size() == cdfs.size());
  assert(sample.size() > 0);

  auto dsize = cdfs.size();
  auto ssize = sample.size();

  std::vector<int> index(dsize, 0);
  for(unsigned int i=0; i<dsize; i++) {
    index[i] = ssize * cdfs[i] - 1;
    if(index[i] < 0) index[i] = 0;
  }

  std::sort(sample.begin(), sample.end());

  for(unsigned int i=0; i<dsize; i++) dist[i] = sample[index[i]];
}

double kl_divergence_with_uniform(const std::vector<uint64_t>& sample) {
  assert(sample.size() > 0);
  auto ssize = sample.size();

  auto sum = (double)(std::reduce(sample.begin(), sample.end()));
  auto log_ssize = std::log((double)(ssize));
  auto minV = (0.0001/sum);
  auto minD = minV * (std::log(minV) + log_ssize);

  double d = 0.0;

  std::for_each(sample.begin(), sample.end(),
                [&](const uint64_t &s){
                  if(s > 0) {
                    double p = (double)(s)/sum;
                    d += p * (std::log(p) + log_ssize);
                  } else
                    d += minD;
                });

  return d;
}
