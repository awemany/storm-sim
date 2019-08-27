// (C)opyright 2019 awemany, see file COPYING for details
#include <iostream>
#include <exception>
#include "scheduler.h"
#include "tinyformat.h"

ScheduledOperation::ScheduledOperation() :
    at_time(0), scheduler(nullptr) {}

ScheduledOperation::~ScheduledOperation() {}

uint64_t ScheduledOperation::time() const { return at_time; }

bool ScheduledOperation::operator<(const ScheduledOperation &other) const {
    return at_time < other.at_time;
}


bool SORefComparator::operator()(const SORef &a, const SORef &b) const {
    // Note: Inverted for priority_queue!
    return *b < *a;
}

bool ScheduledOperation::scheduled() const {
    return scheduler != nullptr;
}

Scheduler::Scheduler(int _min_loglevel) : min_loglevel(_min_loglevel), cur_time(0), cur_count(0) {}

void Scheduler::submit(const uint64_t dt, const SORef& obj) {
    obj->at_time = cur_time + dt;
    obj->scheduler = this;
    queue.push(obj);
}

bool Scheduler::step() {
    if (!queue.size()) return false;
    SORef opsr = queue.top(); queue.pop();
    if (cur_time > opsr->time()) throw std::runtime_error("Internal scheduler problem, noncausual behavior.");
    cur_time = opsr->time();
    (*opsr)();
    cur_count++;
    return true;
}

void Scheduler::run(uint64_t time_limit) {
    size_t s = 0;
    while (step()) {
        s++;
        if (cur_time >= time_limit) return;
    }
}

void Scheduler::log(const int lvl, const std::string &msg) {
    if (lvl >= min_loglevel) {
        tinyformat::printf("%8d %8d %8d %s\n", currentTime(), currentOpsCount(), queue.size(), msg);
    }
}

uint64_t Scheduler::currentTime() const { return cur_time; }
uint64_t Scheduler::currentOpsCount() const { return cur_count; }


