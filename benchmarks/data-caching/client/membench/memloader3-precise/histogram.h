#ifndef HISTOGRAM_H
#define HISTOGRAM_H

#include <string>
#include <stdio.h>
#include <string.h>
#include "config.h"

#define HISTOGRAM_SIZE 10000

class histogram {
private:
	const std::string name;
	int histogram_slots[HISTOGRAM_SIZE];
	int overflow;
	int sample_cnt;

public:
	histogram(const std::string& name) : name(name) {
		for (int i = 0; i < HISTOGRAM_SIZE; i++) {
			histogram_slots[i] = 0;
		}
		overflow = 0;
		sample_cnt = 0;
	}

	void add_sample(double val) {
		sample_cnt++;
		if (sample_cnt > conf.histogram_head && sample_cnt <= conf.histogram_head + conf.histogram_body) {
			if ((int)val < HISTOGRAM_SIZE) {
				histogram_slots[(int)val]++;
			} else {
				overflow++;
			}
		}
	}

	void dump(const std::string& filename) {
		if (sample_cnt < conf.histogram_head + conf.histogram_body) {
			fprintf(stderr, "%s: not enough samples: %d\n", name.c_str(), sample_cnt);
			exit(0);
		}
		FILE *fp = fopen(filename.c_str(), "w");
		if (!fp) {
			fprintf(stderr, "Unable to create output file '%s': %s\n", filename.c_str(), strerror(errno));
			exit(0);
		}
		for (int i = 0; i < HISTOGRAM_SIZE; i++) {
			fprintf(fp, "%d\n", histogram_slots[i]);
		}
		fprintf(fp, "%d\n", overflow);
		fclose(fp);
	}
};

class time_diff_histogram {
private:
	const std::string name;
	histogram hist;
	double scale;

public:
	time_diff_histogram(const std::string& name, const double scale) : name(name), hist(name), scale(scale) { }

	void add_sample(double new_time, double old_time) {
		if (new_time < old_time) {
			fprintf(stderr, "%s: new_time < old_time: 0x%lf, 0x%lf\n", name.c_str(), new_time, old_time);
			exit(0);
		}
		double delta = new_time - old_time;
		hist.add_sample(delta / scale);
	}

	void dump(const std::string& filename) {
		hist.dump(filename);
	}
};

class interval_histogram {
private:
	const std::string name;
	time_diff_histogram td_hist;
	double last_time;
	bool last_time_set;

public:
	interval_histogram(const std::string &name, const double scale) : name(name), td_hist(name, scale) {
		last_time_set = false;
	}

	void add_sample(const double time) {
		if (!last_time_set) {
			last_time = time;
			last_time_set = true;
			return;
		}
		td_hist.add_sample(time, last_time);
		last_time = time;
	}

	void dump(const std::string& filename) {
		td_hist.dump(filename);
	}
};

#endif
