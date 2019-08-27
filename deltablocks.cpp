// (C)opyright 2019 awemany, see file COPYING for details
#include <algorithm>
#include <memory>
#include <stack>
#include <random>
#include <map>
#include <set>
#include "deltablocks.h"
#include "tinyformat.h"

enum LogLevels {
    llTransaction,
    llBlockRequest,
    llBlock,
    llTxnConfirmStats,
    llMine,
    llStats
};

using namespace std;

static mt19937 rng;

uint64_t rnd64() { return rng() + ((uint64_t)rng()<<32); }

static int total_blocks;
static int total_txns;
static int total_doublespends;
static int total_nodes;
static int total_links;

class Transaction;
typedef shared_ptr<Transaction> TransactionRef;

std::set<TransactionRef> traced_txns;

// txn_history_once: Trace and count transactions that are dropped once
// positive: highest confirm. negative: highest drop after confirm
std::map<TransactionRef, int> txn_history_once;

// txn_history_forever: Trace and count transactions that are dropped forever
std::map<TransactionRef, int> txn_history_forever;

std::map<TransactionRef, uint64_t> txn_first_seen_for_stats;
std::map<TransactionRef, std::vector<uint64_t> > txn_first_n_confs;

class NetworkMessage : public ScheduledOperation {
public:
    virtual size_t size()=0;
};
typedef std::shared_ptr<NetworkMessage> NMRef;

class Transaction {
public:
    int number;
    uint64_t txid;
    Transaction() {
        number = total_txns++;
        txid = rnd64();
    }
    Transaction(Transaction& other) { // this creates a doublespend
        //tfm::printf("CREATED DOUBLESPEND\n");
        number = total_txns++;
        total_doublespends++;
        txid = other.txid;
    }
    bool operator<(const Transaction& other) const {
        return txid<other.txid;
    }
    string str() const { return tfm::format("[TXN %d,%d]", number, txid); }
};

class DeltaBlock;
typedef shared_ptr<DeltaBlock> DeltaBlockRef;

static int wpowForSet(const std::set<DeltaBlockRef>& blocks);

typedef std::pair<const DeltaBlock*, const DeltaBlock*> BlockPtrPair;

static std::set<BlockPtrPair> block_compatible;
static std::set<BlockPtrPair> block_incompatible;

class DeltaBlock {
    int number;
    mutable int weakpow;
    mutable bool weakpow_cached;
    string name;
public:
    set<DeltaBlockRef> parents;
    set<TransactionRef> delta_set;
    std::map<uint64_t, TransactionRef> txns;
    DeltaBlock(const string _name="") : weakpow(-1),
                                        weakpow_cached(false),
                                        name(_name) {
        number = total_blocks++;
    }
    int wpow() const {
        if (weakpow_cached) return weakpow;
        weakpow = 1 + wpowForSet(parents);
        weakpow_cached = true;
        return weakpow;
    }


    bool compatible(const DeltaBlock& other) {
        // duplicate transactions are those that are twice in memory with different pointers but same txid
        BlockPtrPair pair_a(this, &other), pair_b(&other, this);

        if (block_compatible.count(pair_a)>0) return true;
        else if (block_incompatible.count(pair_a)>0) return false;
        for (auto txnpair : other.txns) {
            auto other_txn = txnpair.second;
            if (txns.count(other_txn->txid) > 0)
                if (txns[other_txn->txid]->number != other_txn->number) {
                    //tfm::printf("INCOMPATIBLE BLOCKS\n");
                    block_incompatible.insert(pair_a);
                    block_incompatible.insert(pair_b);
                    return false; // duplicate!
                }
        }
        block_compatible.insert(pair_a);
        block_compatible.insert(pair_b);
        return true;
    }
    string str() const {
        return tfm::format("[BLK %d, %d wpow, %d anc, %d txn, %d delta]", number, wpow(),
                           parents.size(), txns.size(), delta_set.size());
    }
};

// maximum number of confirmations to track
static const int MAX_CONF = 10;



int confirms(DeltaBlockRef& block, TransactionRef txn) {
    /* Calculate number of confirms of a transaction at a given weak
       DAG position, which is the number of subsequent blocks (or the given block) it appears in. */
    size_t res=0;
    std::set<DeltaBlockRef> seen;
    std::stack<DeltaBlockRef> todo;
    todo.push(block);
    while (todo.size()) {
        auto b = todo.top(); todo.pop();
        if (seen.count(b)>0) continue;
        seen.insert(b);
        if (b->txns.count(txn->txid) > 0 &&
            b->txns[txn->txid]->number == txn->number) {
            res++;
            if (res >= MAX_CONF) return MAX_CONF;
            for (auto p : b->parents)
                todo.push(p);
        }
    }
    return res;
}

static int wpowForSet(const std::set<DeltaBlockRef>& blocks) {
    std::set<DeltaBlockRef> seen;
    stack<DeltaBlockRef> todo;
    for (auto p : blocks) todo.push(p);
    while (todo.size()) {
        auto block = todo.top(); todo.pop();
        if (seen.count(block) > 0) continue;
        seen.insert(block);
        for (auto p : block->parents)
            todo.push(p);
    }
    return seen.size();
}

class Node;
typedef shared_ptr<Node> NodeRef;

class Link;
typedef shared_ptr<Link> LinkRef;

class Link {
public:
    int number;
    NodeRef sender, receiver;
    double latency;
    double bandwidth;
    size_t link_busy_until;

    Link(NodeRef _sender, NodeRef _receiver,
         double _latency, double _bandwidth) : sender(_sender), receiver(_receiver),
                                               latency(_latency), bandwidth(_bandwidth),
                                               link_busy_until(0) {
        number = total_links++;
    }
    void submit(Scheduler* scheduler, NMRef msg);};


class Node {
public:
    int number;
    set<DeltaBlockRef> known;
    std::set<DeltaBlockRef> tips;
    std::map<DeltaBlockRef, uint64_t> block_arrival_times;
    std::map<uint64_t, TransactionRef> mempool;
    std::set<uint64_t> known_doublespends; // by txn number

    // earliest arrival of a txn at this node. Set to 0 to mark doublespends
    std::map<uint64_t, int64_t> arrival_times;

    // peer connections
    std::map<NodeRef, LinkRef> peers;

    //! Blocks not yet processed
    std::set<DeltaBlockRef> todo_blocks;

    Node() {
        number = total_nodes++;
    }

    DeltaBlockRef bestTip() const {
        int best_wpow = -1;
        DeltaBlockRef best_block;
        for (auto tip : tips) {
            if (tip->wpow() > best_wpow ||
                (tip->wpow() == best_wpow && block_arrival_times.at(tip) < block_arrival_times.at(best_block))) {
                best_block = tip;
                best_wpow = tip->wpow();
            }
        }
        return best_block;
    }

    bool isFullyKnown(DeltaBlockRef& block) {
        std::stack<DeltaBlockRef> todo;
        todo.push(block);
        while (todo.size()) {
            auto block = todo.top(); todo.pop();
            if (known.count(block) == 0) return false;
            for (auto parent : block->parents) todo.push(parent);
        }
        return true;
    }

    DeltaBlockRef bestMiningTemplate() const {
        std::vector<std::set<DeltaBlockRef> > candidate_blocks;
        std::vector<int> candidate_wpows;

        for (auto block : tips) {
            std::set<DeltaBlockRef> s; s.insert(block);
            candidate_blocks.emplace_back(s);
            assert(block->wpow() == wpowForSet(s));
            candidate_wpows.emplace_back(wpowForSet(s));
        }
        //size_t size_before = candidate_blocks.size();

        // merge logic, active if USE_DELTA_BLOCKS_METHOD is set. Else it will fall back to simple chain of longest POW
        bool modified= USE_DELTABLOCKS_METHOD;
        while (modified) {
            modified = false;
            for (size_t i=0; i < candidate_blocks.size(); i++) {
                for (size_t j = i+1; j < candidate_blocks.size(); j++) {
                    bool compatible = true;
                    for (auto a : candidate_blocks[i]) {
                        for (auto b : candidate_blocks[j]) {
                            if (!a->compatible(*b)) {
                                compatible = false;
                                break;
                            }
                        }
                        if (!compatible) break;
                    }
                    if (compatible) {
                        for (auto b : candidate_blocks[j])
                            candidate_blocks[i].insert(b);
                        candidate_wpows[i] = wpowForSet(candidate_blocks[i]);
                        candidate_blocks.erase(candidate_blocks.begin() + j);
                        candidate_wpows.erase(candidate_wpows.begin() + j);
                        modified=true;
                        break;
                    }
                }
                if (modified) break;
            }
        }
        //tfm::printf("MERGE SET SIZE %d\n", candidate_blocks.size());
        int max_wpow = -1;
        std::set<DeltaBlockRef> best_parents;

        for (size_t i = 0; i < candidate_blocks.size(); i++) {
            if (candidate_wpows[i] > max_wpow) {
                max_wpow = candidate_wpows[i];
                best_parents = candidate_blocks[i];
            }
        }
        if (best_parents.size()) {
            auto* block = new DeltaBlock();
            block->parents = best_parents;

            std::stack<DeltaBlockRef> todo;
            std::set<DeltaBlockRef> done;
            for (auto parent : best_parents)
                todo.push(parent);

            while (todo.size()) {
                auto b = todo.top(); todo.pop();
                if (done.count(b) > 0) continue;
                for (auto txn : b->delta_set) {
                    /*assert (block->txns.count(txn->txid) == 0 ||
                      block->txns[txn->txid]->number == txn->number); */
                    block->txns[txn->txid]=txn;
                }
                for (auto parent : b->parents)
                    todo.push(parent);
                done.insert(b);
            }
            return DeltaBlockRef(block);
        } else return DeltaBlockRef(new DeltaBlock("genesis-like"));
    }
    string str() const { return tfm::format("[NODE %d]", number); }
};

void Link::submit(Scheduler* scheduler, NMRef msg) {
    double delay = msg->size() / bandwidth;
    if (link_busy_until < scheduler->currentTime()) {
        link_busy_until = scheduler->currentTime() + delay;
        scheduler->submit(latency + delay, SORef(msg));
    } else {
        uint64_t wait = link_busy_until - scheduler->currentTime();
        scheduler->submit(latency + delay + wait, SORef(msg));
        link_busy_until += delay;
        if (link_busy_until > scheduler->currentTime() + 1*SEC) {
            scheduler->log(llStats, tfm::format("PROBABLE CONTENTION LINK %s %s %f %d %d %f %f",
                                                sender->str(), receiver->str(),
                                                (link_busy_until-scheduler->currentTime())/
                                                (double)SEC, scheduler->currentTime(),
                                                link_busy_until, latency, bandwidth));
        }
    }
}

static uint64_t blockSizeBytes(DeltaBlockRef& block) {
    uint64_t result = DELTA_BLOCK_EMPTY_SIZE;
    result += block->delta_set.size() * TXN_SIZE_BLOCK;
    result += block->parents.size() * ANCESTOR_SIZE;
    return result;
}

static vector<NodeRef> nodes;

void createNodes() {
    switch (NODE_SCENARIO) {
    case scRandom:
        while (total_nodes < NUM_NODES)
            nodes.emplace_back(NodeRef(new Node()));
        break;
    case sc012:
        while (total_nodes < 3)
            nodes.emplace_back(NodeRef(new Node()));
        break;
    default:
        throw std::runtime_error("Invalid node connection scenario.");
    }
}

void connectNodes() {
    size_t num_conn = 0;
    for (auto node : nodes)
        node->peers.clear();
    total_nodes=0;
    total_links=0;

    switch (NODE_SCENARIO) {
    case scRandom:
        while (num_conn < NUM_NODECONNS) {
            uniform_int_distribution<size_t> nodesel(0, nodes.size()-1);
            size_t a = nodesel(rng);
            size_t b = nodesel(rng);

            if (a == b) continue;
            if (nodes[a]->peers.count(nodes[b]) > 0)
                continue;
            normal_distribution<double> lat_dist(MEAN_LATENCY, LATENCY_SIGMA);
            double latency = max(lat_dist(rng), LATENCY_FLOOR);

            normal_distribution<double> bw_dist(MEAN_BANDWIDTH, BANDWIDTH_SIGMA);
            double bandwidth = max(bw_dist(rng), BANDWIDTH_FLOOR);

            nodes[a]->peers[nodes[b]] = LinkRef(new Link(nodes[a], nodes[b], latency, bandwidth));
            nodes[b]->peers[nodes[a]] = LinkRef(new Link(nodes[b], nodes[a], latency, bandwidth));
            tfm::printf("SETUP NODE CONNECTION %d %d %f %f\n", a, b, latency, bandwidth / KILOBYTE * SEC);
            num_conn++;
        }
        break;
    case sc012:
        // 1<->0<-->2 connection
        nodes[0]->peers[nodes[1]] = LinkRef(
            new Link(nodes[0], nodes[1], MEAN_LATENCY, MEAN_BANDWIDTH));
        nodes[1]->peers[nodes[0]] = LinkRef(
            new Link(nodes[1], nodes[0], MEAN_LATENCY, MEAN_BANDWIDTH));
        num_conn++;

        nodes[0]->peers[nodes[2]] = LinkRef(
            new Link(nodes[0], nodes[2], MEAN_LATENCY, MEAN_BANDWIDTH));
        nodes[2]->peers[nodes[0]] = LinkRef(
            new Link(nodes[2], nodes[0], MEAN_LATENCY, MEAN_BANDWIDTH));
        num_conn++;
        tfm::printf("SETUP NODE CONNECTION sc123\n");
        break;
    default:
        throw std::runtime_error("Invalid node connection scenario.");
    }
}

// FIXME: Maybe add expected latency for a given load bandwidth as a parameter?
std::map<NodeRef, double>  shortestPaths(NodeRef from) {
    std::map<NodeRef, double> distances;
    distances[from] = 0;

    typedef class std::pair<double, LinkRef> LinkDistPair;
    std::priority_queue<LinkDistPair, std::vector<LinkDistPair>, std::greater<LinkDistPair> > todo;
    for (auto ppeer : from->peers) {
        auto link = ppeer.second;
        todo.push(LinkDistPair(link->latency, link));
    }

    while (todo.size()) {
        auto x = todo.top(); todo.pop();
        auto full_latency = x.first;
        auto link = x.second;
        if (distances.count(link->receiver) > 0) continue;
        assert(distances.count(link->sender) > 0);
        distances[link->receiver] = full_latency;
        for (auto ppeer : link->receiver->peers) {
            NodeRef peer = ppeer.first;
            LinkRef link = ppeer.second;
            todo.push(LinkDistPair(link->latency + distances[link->sender], link));
       }
    }
    return distances;
}

void printDistanceMetrics() {
    double mean_dist = 0;
    double max_dist = 0;
    NodeRef max_a, max_b;
    size_t n = 0;
    for (auto node : nodes) {
        auto distances = shortestPaths(node);
        for (auto p : distances) {
            auto node2 = p.first;
            auto distance = p.second;
            if (distance > max_dist) {
                max_a = node;
                max_b = node2;
                max_dist = distance;
            }
            // note: does include distance to self
            mean_dist += distance;
            n++;
        }
    }
    tfm::printf("DISTANCE METRICS MAX %f %d %d MEAN %f PAIRS %d\n",
                max_dist, max_a->number, max_b->number, mean_dist/n, n);
}

bool checkFullConnectivity() {
    std::set<NodeRef> seen;
    std::stack<NodeRef> todo;
    todo.push(nodes[0]);
    while (todo.size()) {
        auto node = todo.top(); todo.pop();
        if (seen.count(node)>0) continue;
        seen.insert(node);
        for (auto peerpair : node->peers) {
            auto peer = peerpair.first;
            todo.push(peer);
        }
    }
    //assert (seen.size() == nodes.size());
    return seen.size() == nodes.size();
}

class ReceiveTransaction : public NetworkMessage {
    NodeRef sender, receiver;
    TransactionRef txn;
public:
    ReceiveTransaction(NodeRef _sender,
                       NodeRef _receiver,
                       TransactionRef& _txn) : sender(_sender), receiver(_receiver), txn(_txn) {}

    size_t size() { return TXN_SIZE; }
    void operator()() {
        if (receiver->number == TXN_TRACE_NODE && traced_txns.count(txn) > 0 &&
            txn_first_seen_for_stats.count(txn) == 0)
            txn_first_seen_for_stats[txn] = time();

        std::string sender_str = sender == nullptr ? "null" : sender->str();
        /* if (receiver->number == TXN_TRACE_NODE && traced_txns.count(txn) > 0) {
            scheduler->log(llTxnConfirmStats,
                           tfm::format("TRACED TXN %s %s", receiver->str(), txn->str()));
                           }*/

        const bool do_gossip = (receiver->mempool.count(txn->txid) == 0 ||
                                (RELAY_DOUBLESPENDS && receiver->known_doublespends.count(txn->number) == 0 &&
                                 receiver->mempool[txn->txid]->number != txn->number));

        scheduler->log(llTransaction, tfm::format("RX TXN %s %s %s %d", sender_str, receiver->str(),
                                                  txn->str(), do_gossip));

        if (receiver->mempool.count(txn->txid) > 0) {
            if (receiver->mempool[txn->txid]->number != txn->number) {
                receiver->known_doublespends.insert(txn->number);
                if (MINERS_DROP_KNOWN_DOUBLESPEND) {
                    // this will prevent the txn from being mined at all
                    scheduler->log(llTransaction, tfm::format("MARK DOUBLESPEND %s %s %s", receiver->str(),
                                                              receiver->mempool[txn->txid]->str(), txn->str()));
                    receiver->arrival_times[txn->txid] = 0;
                }
            }
        } else {
            receiver->mempool[txn->txid]=txn;
            receiver->arrival_times[txn->txid] = (int64_t)time();
        }

        if (do_gossip) {
            for (auto p : receiver->peers) {
                auto pto = p.first;
                auto link = p.second;
                if (pto != sender)
                    link->submit(scheduler, NMRef(new ReceiveTransaction(receiver, pto, txn)));
            }
        }
    }
};

class ReceiveDeltaBlock;

class RequestBlock : public NetworkMessage {
    NodeRef sender, receiver;
    DeltaBlockRef block;
public:
    RequestBlock(NodeRef _sender,
                 NodeRef _receiver,
                 DeltaBlockRef& _block) : sender(_sender), receiver(_receiver), block(_block) {}

    size_t size() { return BLOCK_REQUEST_SIZE; }
    void operator()();
};

static void gossip(Scheduler* scheduler, NodeRef originator, NodeRef from, DeltaBlockRef& block);

class ReceiveDeltaBlock : public NetworkMessage {
    NodeRef sender, receiver;
    DeltaBlockRef block;
public:
    ReceiveDeltaBlock(NodeRef _sender, NodeRef _receiver, DeltaBlockRef& _block) :
        sender(_sender), receiver(_receiver), block(_block) {}
    size_t size() { return blockSizeBytes(block); }
    void operator()() {
        if (receiver->known.count(block)>0 || receiver->todo_blocks.count(block) > 0) return;

        if (sender != nullptr &&
            uniform_real_distribution<double>(0.0, 1.0)(rng) < BLOCK_REREQUEST_PROBABILITY) {
            auto link = receiver->peers[sender];
            assert (link != nullptr);
            link->submit(scheduler, NMRef(new RequestBlock(receiver, sender, block)));
            return;
        }
        receiver->todo_blocks.insert(block);

        bool done = false;
        while (! done) {
            done = true;
            for (auto block : receiver->todo_blocks) {
                bool process = true;
                for (auto parent : block->parents) {
                    if (receiver->known.count(parent) == 0) {
                        process=false;
                        break;
                    }
                }
                if (process) {
                    processBlock(block);
                    done = false;
                    receiver->todo_blocks.erase(block);
                    break;
                }
            }
        }
    }

    void processBlock(DeltaBlockRef &block) {
        std::string sender_str = sender == nullptr ? "null" : sender->str();
        scheduler->log(llBlock, tfm::format("RX BLK %s %s %s %d %d %d",
                                            sender_str, receiver->str(), block->str(), total_txns, total_blocks,
                                            receiver->todo_blocks.size()));
        receiver->known.insert(block);
        receiver->tips.insert(block);
        receiver->block_arrival_times[block]=time();
        for (auto b : block->parents)
            receiver->tips.erase(b);

        if (receiver->number == TXN_TRACE_NODE && traced_txns.size()) {
            DeltaBlockRef best_tip = receiver->bestTip();
            for (auto txn : traced_txns) {
                /*scheduler->log(llTxnConfirmStats,
                  tfm::format("TRACED TXN CONFIRMS BEST TIP %s %s %d",
                  receiver->str(), txn->str(), confirms(best_tip, txn)));*/

                int c = confirms(best_tip, txn);

                if (txn_first_n_confs.count(txn) == 0)
                    txn_first_n_confs[txn].resize(MAX_CONF, 0);

                if (c < MAX_CONF && txn_first_n_confs[txn][c] == 0)
                    txn_first_n_confs[txn][c] = time();

                if (txn_history_once.count(txn) == 0) {
                    txn_history_once[txn] =c;
                } else {
                    // lost once, lost forever assumption
                    if (txn_history_once[txn] >=0) {
                        if (c >= txn_history_once[txn])
                            txn_history_once[txn] = c;
                        else if (c==0)
                            // this value will stick
                            txn_history_once[txn] = -txn_history_once[txn];
                    }
                }
                if (txn_history_forever.count(txn) == 0) {
                    txn_history_forever[txn] =c;
                } else {
                    // txn can go back in..
                    if (c >= abs(txn_history_forever[txn]))
                        txn_history_forever[txn] = c;
                    else if (c==0 && txn_history_forever[txn] > 0)
                        txn_history_forever[txn] = -txn_history_forever[txn];
                }
            }
            scheduler->log(llTxnConfirmStats,
                           tfm::format("TRACED CONFIRM STATS TOTALS %s %d %d %d %d",
                                       best_tip->str(), total_txns, total_doublespends, total_blocks,
                                       traced_txns.size()));

            int seen_confs_but_dropped[MAX_CONF];
            for (size_t i = 0; i < MAX_CONF; i++)
                seen_confs_but_dropped[i]=0;

            for (auto txn : traced_txns) {
                int c = txn_history_once[txn];
                if (c<0) {
                    seen_confs_but_dropped[min(-c, MAX_CONF-1)]++;
                }
            }
            for (size_t i = 0; i < MAX_CONF; i++)
                scheduler->log(llTxnConfirmStats,
                               tfm::format("TRACED CONFIRM STATS MAX CONF DROPPED ONCE %d %d",
                                           i, seen_confs_but_dropped[i]));

            for (size_t i = 0; i < MAX_CONF; i++)
                seen_confs_but_dropped[i]=0;
            for (auto txn : traced_txns) {
                int c = txn_history_forever[txn];
                if (c<0) {
                    seen_confs_but_dropped[min(-c, MAX_CONF-1)]++;
                }
            }
            for (size_t i = 0; i < MAX_CONF; i++)
                scheduler->log(llTxnConfirmStats,
                               tfm::format("TRACED CONFIRM STATS MAX CONF DROPPED FOREVER %d %d",
                                           i, seen_confs_but_dropped[i]));
        }
        gossip(scheduler, sender, receiver, block);
    }
};

void RequestBlock::operator()() {
    scheduler->log(llBlock,
                   tfm::format("REQUEST BLK %s %s %s", sender->str(), receiver->str(),
                               block->str()));
    auto link = receiver->peers[sender];
    assert (link != nullptr);
    link->submit(scheduler,
                                    NMRef(new ReceiveDeltaBlock(receiver, sender, block)));
}

static void gossip(Scheduler* scheduler, NodeRef originator, NodeRef from, DeltaBlockRef& block) {
    for (auto p  : from->peers) {
        auto peer = p.first;
        auto link = p.second;
        if (peer != originator)
            link->submit(scheduler, NMRef(new ReceiveDeltaBlock(from, peer, block)));
    }
}

static uint64_t miningTime() {
    std::exponential_distribution<double> expd(1./WEAK_BLOCK_TIME);
    return expd(rng);
}

static uint64_t txnTime() {
    std::exponential_distribution<double> expd(1./(TRANSACTION_TIME*(1.+DOUBLESPEND_RATE)));
    return expd(rng);
}


class MineBlock : public ScheduledOperation {
public:
    MineBlock() {}
    void operator()() {
        uniform_int_distribution<size_t> nodesel(0, nodes.size()-1);
        NodeRef miner = nodes[nodesel(rng)];
        DeltaBlockRef block = miner->bestMiningTemplate();

        std::set<uint64_t> to_remove;

        size_t lt_minage=0, kn_ds = 0;

        for (auto txnpair : miner->mempool) {
            auto txid = txnpair.first;
            auto txn = txnpair.second;
            if (miner->arrival_times[txid] == 0) {
                // known double spend -> ignore
                kn_ds++;
                continue;
            } else if (time() - miner->arrival_times[txid] < TXN_MEMPOOL_MIN_AGE) {
                // transaction not old enough for this miner
                lt_minage++;
                continue;
            } else if (time() - miner->arrival_times[txid] > TXN_MEMPOOL_MAX_AGE) {
                to_remove.insert(txid);
                continue;
            }

            if (block->txns.count(txid) == 0) {
                block->txns[txid]=txn;
                block->delta_set.insert(txn);
            }
        }
        scheduler->log(llMine, tfm::format("MINED BLK %s %s %d %d %d %d", miner->str(), block->str(),
                                           miner->mempool.size(),
                                           lt_minage, kn_ds,
                                           to_remove.size()));

        if (1) {
            for (auto tx : block->delta_set) {
                scheduler->log(llBlock, tfm::format("BLK %s CONTAINS %s", block->str(), tx->str()));
            }
            for (auto par : block->parents) {
                scheduler->log(llBlock, tfm::format("BLK %s PARENT %s", block->str(),
                                                    par->str()));
            }
        }

        for (auto txid : to_remove) {
            miner->mempool.erase(txid);
            miner->arrival_times.erase(txid);
        }
        scheduler->submit(0, SORef(new ReceiveDeltaBlock(nullptr, miner, block)));
        scheduler->submit(miningTime(), SORef(new MineBlock()));
    }
};

class MakeTransaction : public ScheduledOperation {
public:
    MakeTransaction() {}
    void operator()() {
        uniform_int_distribution<size_t> nodesel(0, nodes.size()-1);
        NodeRef receiver = nodes[nodesel(rng)];

        TransactionRef txn = TransactionRef(new Transaction());
        scheduler->log(llTransaction, tfm::format("GEN TXN %s %s", receiver->str(), txn->str()));

        if (uniform_real_distribution<double>(0.0, 1.0)(rng) < TXN_TRACE_PROBABILITY)
          traced_txns.insert(txn);
        uniform_real_distribution<double> dsd(0.0, 1.0);

        scheduler->submit(0, SORef( new ReceiveTransaction(nullptr, receiver, txn)));

        if (dsd(rng) < DOUBLESPEND_RATE) {
             // this Transaction(..) constructor creates a doublespend
            TransactionRef ds_txn = TransactionRef(new Transaction(*txn));
            NodeRef ds_receiver = nodes[nodesel(rng)];
            scheduler->log(llTransaction, tfm::format("GEN DOUBLESPEND TXN %s %s", ds_receiver->str(), ds_txn->str()));
            if (uniform_real_distribution<double>(0.0, 1.0)(rng) < TXN_TRACE_PROBABILITY)
                traced_txns.insert(ds_txn);
            scheduler->submit(0, SORef( new ReceiveTransaction(nullptr, ds_receiver, ds_txn)));
        }
        scheduler->submit(txnTime(), SORef(new MakeTransaction()));
    }
};

class StatsTips : public ScheduledOperation {
public:
    StatsTips() {}
    void operator()() {
        for (NodeRef node : nodes) {
            DeltaBlockRef best_tip = node->bestTip();
            if (best_tip == nullptr)
                scheduler->log(llStats, tfm::format("STAT TIPS %s %d %d %d %d %d null",
                                                    node->str(), node->tips.size(), -1,
                                                    total_txns, total_doublespends, total_blocks));
            else
                scheduler->log(llStats, tfm::format("STAT TIPS %s %d %d %d %d %d %s",
                                                    node->str(), node->tips.size(), best_tip->wpow(),
                                                    total_txns, total_doublespends, total_blocks, best_tip->str()));
        }
        scheduler->submit(100*MILLISEC, SORef(new StatsTips()));
    }
};

class StatsEqualTips : public ScheduledOperation {
public:
    StatsEqualTips() {}
    void operator()() {
        std::map<DeltaBlockRef, int> tip_count;
        int max_tip_count = 0;

        for (NodeRef node : nodes) {
            auto best_tip = node->bestTip();
            if (tip_count.count(best_tip))
                tip_count[best_tip]++;
            else tip_count[best_tip] = 1;
            if (tip_count[best_tip] > max_tip_count)
                max_tip_count = tip_count[best_tip];
        }
        scheduler->log(llStats, tfm::format("STAT EQUAL TIPS %d %d %d", max_tip_count, tip_count.size(), nodes.size()));
        scheduler->submit(100*MILLISEC, SORef(new StatsEqualTips()));
    }
};

void printTxnFirstConfirmStats(void) {
    for (auto p1 : txn_first_n_confs) {
        auto txn = p1.first;
        auto first_confirm_times = p1.second;
        tfm::printf("FIRST CONFIRM STAT %s %d ", txn->str(), txn_first_seen_for_stats[txn]);
        for (auto nconf : first_confirm_times)
            tfm::printf("%d ", nconf);
        tfm::printf("\n");
    }
}
Scheduler* deltablocksSetup(uint64_t seed) {
    rng.seed(seed);
    createNodes();
    connectNodes();
    while(!checkFullConnectivity()) {
        tfm::printf("REDOING NODE CONNECTIONS NOT A CONNECTED GRAPH\n");
        connectNodes();
    }
    printDistanceMetrics();
    Scheduler* scheduler = new Scheduler(llTransaction);
    scheduler->submit(miningTime(), SORef(new MineBlock()));
    scheduler->submit(txnTime(), SORef(new MakeTransaction()));
    scheduler->submit(0*MILLISEC, SORef(new StatsTips()));
    scheduler->submit(0*MILLISEC, SORef(new StatsEqualTips()));
    return scheduler;
}
