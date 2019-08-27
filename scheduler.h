#ifndef SDB_SCHEDULER_H
#define SDB_SCHEDULER_H
// (C)opyright 2019 awemany, see file COPYING for details

#include <memory>
#include <queue>

const uint64_t MICROSEC = 1;
const uint64_t MILLISEC = 1000 * MICROSEC;
const uint64_t SEC = 1000 * MILLISEC;

class Scheduler;

class ScheduledOperation {
    friend class Scheduler;
    friend class SORefComp;
public:
    ScheduledOperation();
    virtual ~ScheduledOperation();
    uint64_t time() const;

    bool operator<(const ScheduledOperation &other) const;
    virtual void operator()()=0;
    bool scheduled() const;
private:
    uint64_t at_time;
protected:
    Scheduler *scheduler;
};

typedef std::shared_ptr<ScheduledOperation> SORef;

class SORefComparator {
public:
    bool operator()(const SORef &a, const SORef &b) const;
};

class Scheduler {
public:
    Scheduler(int _min_loglevel);
    void submit(const uint64_t dt, const SORef& obj);
    bool step();
    void run(uint64_t time_limit);
    uint64_t currentTime() const;
    uint64_t currentOpsCount() const;
    void log(const int lvl, const std::string& msg);
private:
    int min_loglevel;
    uint64_t cur_time;
    uint64_t cur_count;
    std::priority_queue<SORef, std::vector<SORef>, SORefComparator> queue;
};

#endif
