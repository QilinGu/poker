// Compute exact winning probabilities for all preflop holdem matchups

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include <tr1/unordered_map>
#include <algorithm>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "cl.hpp"
#include <omp.h>
#include <getopt.h>
#include "score.h"

using std::ostream;
using std::cin;
using std::cout;
using std::cerr;
using std::endl;
using std::flush;
using std::vector;
using std::string;
using std::min;
using std::max;
using std::tr1::unordered_map;
using std::make_pair;
using std::pair;
using std::sort;

namespace {

const char show_card[13+1] = "23456789TJQKA";
const char show_suit[4+1] = "shdc";

bool do_nothing = false;

cards_t read_cards(const char* s) {
    size_t n = strlen(s);
    assert(!(n&1));
    n /= 2;
    cards_t cards = 0;
    for (size_t i = 0; i < n; i++) {
        const char* p = strchr(show_card,s[2*i]);
        assert(p);
        const char* q = strchr(show_suit,s[2*i+1]);
        assert(q);
        cards |= cards_t(1)<<((p-show_card)+13*(q-show_suit));
    }
    return cards;
}

string show_cards(cards_t cards) {
    string r;
    for (int c = 0; c < 13; c++)
        for (int s = 0; s < 4; s++)
            if (cards&(cards_t(1)<<(c+13*s))) {
                r += show_card[c];
                r += show_suit[s];
            }
    return r;
}

template<class I> string binary(I x, int n = 13, bool pad = false) {
    string s = "0b";
    bool on = pad;
    for (int i = 8*sizeof(I)-1; i >= 0; i--) {
        if (on || x&I(1)<<i) {
            s += x&I(1)<<i?'1':'0';
            on = true;
        }
        if (on && i%n==0 && i)
            s += ',';
    }
    if (s.size()==2)
        s += '0';
    return s;
}

struct hand_t {
    unsigned card0 : 4;
    unsigned card1 : 4;
    unsigned suited : 1;

    hand_t(int card0,int card1,bool suited)
        :card0(card0),card1(card1),suited(suited) {}

    bool operator==(const hand_t h) {
        return card0==h.card0 && card1==h.card1 && suited==h.suited;
    }

    friend ostream& operator<<(ostream& out, hand_t hand) {
        out<<show_card[hand.card0]<<show_card[hand.card1];
        if (hand.card0!=hand.card1)
            out<<(hand.suited?'s':'o');
        return out;
    }
};

inline uint64_t popcount(uint64_t x) {
    return __builtin_popcountl(x);
}

const char* show_type(score_t type) {
    switch (type) {
        case HIGH_CARD:      return "high-card";
        case PAIR:           return "pair";
        case TWO_PAIR:       return "two-pair";
        case TRIPS:          return "trips";
        case STRAIGHT:       return "straight";
        case FLUSH:          return "flush";
        case FULL_HOUSE:     return "full-house";
        case QUADS:          return "quads";
        case STRAIGHT_FLUSH: return "straight-flush";
        default:             return "<unknown>";
    }
}

// The current timer implementation isn't thread safe, so we disable it if using more than one device
bool disable_timing = false;

class timer_t {
    // We store timing information in a tree of named nodes
    typedef int node_id;
    static node_id next_id;
    static unordered_map<node_id,unordered_map<const char*,node_id> > children;
    static unordered_map<node_id,double> time;
    static node_id current;
    // Information about the current timer scope
    node_id parent;
    double start;
public:
    timer_t(const char* name) {
        if (disable_timing) {
            parent = start = 0;
            return;
        }
        start = current_time();
        parent = current;
        node_id& self = children[current][name]; 
        if (!self)
            self = ++next_id;
        current = self;
    }

    ~timer_t() {
        close();
    }

    void close() {
        if (disable_timing || parent<0)
            return;
        double end = current_time();
        time[current] += end-start;
        current = parent;
    }

    static void dump() {
        if (disable_timing)
            return;
        cerr<<"Timing:"<<endl; 
        int width = 0;
        dump_width(1,0,width);
        dump_helper(1,0,width);
        fflush(stderr);
    }

private:
    static double current_time() {
        timeval tv;
        gettimeofday(&tv,0);
        return tv.tv_sec+1e-6*tv.tv_usec;
    }

    static void dump_width(int depth, node_id node, int& width) {
        const unordered_map<const char*,node_id>& c = children[node];
        vector<pair<node_id,const char*> > ids;
        for (unordered_map<const char*,node_id>::const_iterator i = c.begin(), e = c.end(); i != e; ++i) {
            width = max(width,int(2*depth+strlen(i->first)));
            dump_width(depth+1,i->second,width);
        }
    }

    static void dump_helper(int depth, node_id node, int width) {
        const unordered_map<const char*,node_id>& c = children[node];
        vector<pair<node_id,const char*> > ids;
        for (unordered_map<const char*,node_id>::const_iterator i = c.begin(), e = c.end(); i != e; ++i)
            ids.push_back(make_pair(i->second,i->first));
        sort(ids.begin(),ids.end());
        double total = time[node];
        for (size_t i = 0; i < ids.size(); i++) {
            double t = time[ids[i].first];
            fprintf(stderr,"%*s%-*s%8.4f s\n",2*depth,"",width-2*depth,ids[i].second,t);
            dump_helper(depth+1,ids[i].first,width);
            total -= t;
        }
        if (ids.size() && time[node])
            fprintf(stderr,"%*s%-*s%8.4f s\n",2*depth,"",width-2*depth,"other",total);
    }
};
timer_t::node_id timer_t::next_id = 0;
timer_t::node_id timer_t::current = 0;
unordered_map<timer_t::node_id,unordered_map<const char*,timer_t::node_id> > timer_t::children;
unordered_map<timer_t::node_id,double> timer_t::time;

struct outcomes_t {
    uint32_t alice,bob,tie;

    outcomes_t()
        :alice(0),bob(0),tie(0) {}

    outcomes_t& operator+=(outcomes_t o) {
        alice+=o.alice;bob+=o.bob;tie+=o.tie;
        return *this;
    }

    uint32_t total() const {
        return alice+bob+tie;
    }
};

// To make parallelization easy, we precompute the set of 5 element subsets of 48 elements.
five_subset_t five_subsets[NUM_FIVE_SUBSETS];

void compute_five_subsets() {
    int n = 0;
    for (int i0 = 0; i0 < 48; i0++)
        for (int i1 = 0; i1 < i0; i1++)
            for (int i2 = 0; i2 < i1; i2++)
                for (int i3 = 0; i3 < i2; i3++)
                    for (int i4 = 0; i4 < i3; i4++)
                        five_subsets[n++] = i0|i1<<6|i2<<12|i3<<18|i4<<24;
}

// OpenCL information
cl::Context context;
cl::Program program;
struct device_t {
    cl::Device id;
    cl::CommandQueue queue;
    cl::Kernel score_hands;
    cl::Buffer cards;
    cl::Kernel compare_cards;
    cl::Buffer five_subsets;
    cl::Buffer free;
    cl::Buffer results;
    cl::Kernel hash_scores;

    bool operator<(const device_t& d) const {
        return type()==CL_DEVICE_TYPE_GPU && d.type()!=CL_DEVICE_TYPE_GPU;
    }

    int type() const {
        return id.getInfo<CL_DEVICE_TYPE>();
    }
};
vector<device_t> devices;

const size_t max_cards = 20<<17;
const size_t result_space = max(sizeof(score_t)*max_cards,sizeof(uint64_t)*NUM_FIVE_SUBSETS/BLOCK_SIZE);

void initialize_opencl(int device_types, bool verbose=true) {
    timer_t timer("opencl");
    // Allocate context
    context = cl::Context(device_types,0);

    // Query all available devices
    vector<cl::Device> ids = context.getInfo<CL_CONTEXT_DEVICES>(); 
    if (!ids.size()) {
        cerr<<"error: no available OpenCL devices"<<endl;
        exit(1);
    }
    devices.resize(ids.size());
    if (devices.size()>1)
        disable_timing = true;
    for (size_t i = 0; i < devices.size(); i++)
        devices[i].id = ids[i];
    sort(devices.begin(),devices.end()); // Sort GPUs first
    if (verbose) {
        cerr<<"found "<<devices.size()<<" opencl "<<(devices.size()==1?"device: ":"devices: ");
        for (size_t i = 0; i < devices.size(); i++)
            cerr<<(i?", ":"")<<devices[i].id.getInfo<CL_DEVICE_NAME>();
        cerr<<endl;
    }

    // Load and build the program
    FILE* file = fopen("score.cl","r");
    if (!file) {
        cerr<<"error: couldn't open \"score.cl\" for reading"<<endl;
        exit(1);
    }
    struct stat st;
    stat("score.cl",&st);
    string source(st.st_size+1,0);
    fread(&source[0],st.st_size,1,file);
    fclose(file);
    char options[2048] = "-Werror -I";
    getcwd(options+strlen(options),2048-strlen(options));
    cl::Program::Sources sources(1,make_pair(source.c_str(),strlen(source.c_str())));
    program = cl::Program(context,sources);
    {
        timer_t timer("build");
        int status = program.build(ids,options);
        if (status!=CL_SUCCESS) {
            assert(status==CL_BUILD_PROGRAM_FAILURE);
            cerr<<"error: failed to build opencl code"<<endl;
            for (size_t i = 0; i < devices.size(); i++)
                cerr<<"device "<<i<<":\n"<<program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(devices[i].id)<<flush;
            exit(1);
        }
    }

    // Set up each device
    for (size_t i = 0; i < devices.size(); i++) {
        device_t& d = devices.at(i);
        // Make a command queue
        d.queue = cl::CommandQueue(context,d.id);
        // Make the kernels
        d.score_hands = cl::Kernel(program,"score_hands_kernel");
        d.compare_cards = cl::Kernel(program,"compare_cards_kernel",0);
        d.hash_scores = cl::Kernel(program,"hash_scores_kernel",0);
        // Allocate device arrays
        assert(max_cards*sizeof(score_t)<=result_space);
        d.cards = cl::Buffer(context,CL_MEM_READ_ONLY,max_cards*sizeof(cards_t));
        d.five_subsets = cl::Buffer(context,CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,sizeof(five_subsets),five_subsets);
        d.free = cl::Buffer(context,CL_MEM_READ_ONLY,48*sizeof(cards_t));
        d.results = cl::Buffer(context,CL_MEM_WRITE_ONLY,result_space);
        // Set constant parameters
        d.score_hands.setArg(0,d.cards);
        d.score_hands.setArg(1,d.results);
        d.compare_cards.setArg(0,d.five_subsets);
        d.compare_cards.setArg(1,d.free);
        d.compare_cards.setArg(2,d.results);
        d.hash_scores.setArg(0,d.results);
    }
}

// Score a bunch of hands in parallel using OpenCL
void score_hands_opencl(size_t device, size_t n, score_t* scores, const cards_t* cards) {
    assert(n <= max_cards);
    const device_t& d = devices.at(device);
    size_t count = (n+3)/4;
    d.queue.enqueueWriteBuffer(d.cards,CL_TRUE,0,n*sizeof(cards_t),cards);
    d.queue.enqueueNDRangeKernel(d.score_hands,cl::NullRange,cl::NDRange(count),cl::NullRange);
    d.queue.enqueueReadBuffer(d.results,CL_TRUE,0,n*sizeof(score_t),scores);
}

// Hash a bunch of hands in parallel using OpenCL
void hash_scores_opencl(size_t device, size_t n, uint64_t* hashes) {
    assert(sizeof(uint64_t)*n <= result_space);
    device_t& d = devices.at(device);
    const size_t batch = 1<<14;
    for (size_t i = 0; i < n; i += batch) {
        size_t count = min(batch,n-i);
        d.hash_scores.setArg(1,i);
        d.queue.enqueueNDRangeKernel(d.hash_scores,cl::NullRange,cl::NDRange(count),cl::NullRange);
        d.queue.enqueueReadBuffer(d.results,CL_TRUE,0,count*sizeof(uint64_t),hashes+i);
        if ((n/batch)%max(size_t(1),n/batch/1024)==0)
            cout<<'.'<<flush;
    }
    cout<<endl;
}

// Process all five subsets in parallel using OpenCL
uint64_t compare_cards_opencl(size_t device, cards_t alice_cards, cards_t bob_cards, const cards_t* free) {
    device_t& d = devices.at(device);
    // Set arguments
    {timer_t timer("set args");
    d.compare_cards.setArg(3,alice_cards);
    d.compare_cards.setArg(4,bob_cards);}
    // Copy free to device
    {timer_t timer("write free");
    d.queue.enqueueWriteBuffer(d.free,CL_TRUE,0,48*sizeof(cards_t),free);}
    // Compute
    const size_t n = NUM_FIVE_SUBSETS/BLOCK_SIZE;
    //const size_t n = (NUM_FIVE_SUBSETS+BLOCK_SIZE-1)/BLOCK_SIZE;
    {timer_t timer("compute");
    d.queue.enqueueNDRangeKernel(d.compare_cards,cl::NullRange,cl::NDRange(n),cl::NullRange);
    d.queue.finish();}
    // Read back results and sum
    vector<uint64_t> results(n);
    {timer_t timer("read results");
    d.queue.enqueueReadBuffer(d.results,CL_TRUE,0,sizeof(uint64_t)*n,&results[0]);}
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += results[i];
    // Fill in missing entries
    {timer_t timer("missing");
    for (size_t i = 0; i < NUM_FIVE_SUBSETS-n*BLOCK_SIZE; i++)
        sum += compare_cards(alice_cards,bob_cards,free,five_subsets[n*BLOCK_SIZE+i]);}
    return sum;
}

inline uint32_t bit_stack(bool b0, bool b1, bool b2, bool b3) {
    return b0|b1<<1|b2<<2|b3<<3;
}

uint64_t total_comparisons = 0;

// Consider all possible sets of shared cards to determine the probabilities of wins, losses, and ties.
// For efficiency, the set of shared cards is generated in decreasing order (this saves a factor of 5! = 120).
outcomes_t compare_hands(size_t device, hand_t alice, hand_t bob) {
    timer_t timer("compare hands");
    uint32_t total = 0;
    uint64_t wins = 0;
    uint64_t cache[16] = {0}; // Cache wins based on 4 suit equality bits
    // We fix the suits of Alice's cards
    const int sa0 = 0, sa1 = !alice.suited;
    const cards_t alice_cards = (cards_t(1)<<(alice.card0+13*sa0))|(cards_t(1)<<(alice.card1+13*sa1));
    // Consider all compatible suits of Bob's cards
    for (int sb0 = 0; sb0 < 4; sb0++)
        for (int sb1 = 0; sb1 < 4; sb1++)
            if ((sb0==sb1)==bob.suited) {
                const cards_t bob_cards = (cards_t(1)<<(bob.card0+13*sb0))|(cards_t(1)<<(bob.card1+13*sb1));
                const cards_t hand_cards = alice_cards|bob_cards;
                // Make sure we don't use the same card twice
                if (popcount(hand_cards)<4) continue;
                // Did we already do this one?
                int sig = bit_stack(sa0==sb0,sa0==sb1,sa1==sb0,sa1==sb1);
                if (!cache[sig]) {
                    // Make a list of the cards we're allowed to use
                    cards_t free[48];
                    for (int c = 0, i = 0; c < 52; c++)
                        if (!((cards_t(1)<<c)&hand_cards))
                            free[i++] = cards_t(1)<<c;
                    // Consider all possible sets of shared cards
                    cache[sig] = do_nothing?1:compare_cards_opencl(device, alice_cards, bob_cards, free);
                    #pragma omp critical
                    total_comparisons += NUM_FIVE_SUBSETS; 
                }
                wins += cache[sig];
                total += NUM_FIVE_SUBSETS;
            }
    // Done
    outcomes_t o;
    o.alice = wins>>32;
    o.bob = uint32_t(wins);
    o.tie = total-o.alice-o.bob;
    return o;
}

void show_comparison(hand_t alice, hand_t bob,outcomes_t o) {
    if (do_nothing) return;
    cout<<alice<<" vs. "<<bob<<":\n"
          "  Alice: "<<o.alice<<"/"<<o.total()<<" = "<<(double)o.alice/o.total()
        <<"\n  Bob:   "<<o.bob<<"/"<<o.total()<<" = "<<(double)o.bob/o.total()
        <<"\n  Tie:   "<<o.tie<<"/"<<o.total()<<" = "<<(double)o.tie/o.total()<<endl;
    if (alice==bob && o.alice!=o.bob) {
        cerr<<"  Error: Identical hands should win equally often"<<endl;
        exit(1);
    }
}

void test_score_hand() {
    const char *Alice = "Alice", *tie = "tie", *Bob = "Bob";
    struct test_t {
        const char *alice,*bob,*shared;
        score_t alice_type,bob_type;
        const char* result;
    };
    const test_t tests[] = {
        {"As2d","KsTc","Qh3h7h9d4c",HIGH_CARD,HIGH_CARD,Alice},         // high card wins
        {"Ks2d","AsTc","Qh3h7h9d4c",HIGH_CARD,HIGH_CARD,Bob},           // high card wins
        {"4s2d","5s3c","QhAh7h9dTc",HIGH_CARD,HIGH_CARD,tie},           // only five cards matter
        {"4s3d","5s3c","QhAh7h9d2c",HIGH_CARD,HIGH_CARD,Bob},           // the fifth card matters
        {"4s3d","4d3c","QhAh7h9d2c",HIGH_CARD,HIGH_CARD,tie},           // suits don't matter
        {"As2d","KsTc","Qh3h7h9d2c",PAIR,HIGH_CARD,Alice},              // pair beats high card
        {"Ks2d","AsTc","Qh3h7h9d2c",PAIR,HIGH_CARD,Alice},              // pair beats high card
        {"Ks2d","AsTc","KhAh7h9d3c",PAIR,PAIR,Bob},                     // higher pair wins
        {"Ks2d","KdTc","KhAh7h9d3c",PAIR,PAIR,Bob},                     // pair + higher kicker wins
        {"KsTd","Kd2c","KhAh7h9d3c",PAIR,PAIR,Alice},                   // pair + higher kicker wins
        {"Ks3d","Kd2c","KhAh7h9d6c",PAIR,PAIR,tie},                     // given a pair, only three other cards matter
        {"7s6d","5d4c","KhKdJh9d8c",PAIR,PAIR,tie},                     // given a pair, only three other cards matter
        {"7s6d","5d4c","7d5h4hAdKc",PAIR,TWO_PAIR,Bob},                 // two pair beats higher pair
        {"2s6d","5d4c","2d5h4hAdKc",PAIR,TWO_PAIR,Bob},                 // two pair beats lower pair
        {"7s2d","5d4c","2h5h4h7dKc",TWO_PAIR,TWO_PAIR,Alice},           // the higher pair matters
        {"7s2d","7d2c","2h5h4h7hKc",TWO_PAIR,TWO_PAIR,tie},             // two pairs can tie
        {"7sAd","7dQc","Kh5h4h7hKc",TWO_PAIR,TWO_PAIR,Alice},           // two pair + higher kicker wins
        {"KsAd","QdAc","JhJcThTc2c",TWO_PAIR,TWO_PAIR,tie},             // only one kicker matters with two pair
        {"JsAd","QdAc","AhJcKhKc2c",TWO_PAIR,TWO_PAIR,Bob},             // three pair doesn't matter
        {"JsAd","QdKc","JhJcQhKs2c",TRIPS,TWO_PAIR,Alice},              // trips beat two pair
        {"JsAd","QdKc","ThTcTs3s2c",TRIPS,TRIPS,Alice},                 // trips + highest kicker wins
        {"9s8d","7d6c","ThTcTsAsKc",TRIPS,TRIPS,tie},                   // only two kickers matter with trips
        {"Ts8d","QdJc","ThTc2sAsKc",TRIPS,STRAIGHT,Bob},                // straight beats trips
        {"Ts8d","QdJc","2h3c4s5s6c",STRAIGHT,STRAIGHT,tie},             // kickers don't matter with straights
        {"Ah5c","Tc2h","6d7h8c9dAs",STRAIGHT,STRAIGHT,Bob},             // highest straight wins
        {"AhJc","5cKh","2d3h4c5d5h",STRAIGHT,TRIPS,Alice},              // aces can be low in straights
        {"AhJc","6cKh","2d3h4c5d5h",STRAIGHT,STRAIGHT,Bob},             // the wheel is the lowest straight
        {"AhJc","6c2d","Th3h4h5d5h",FLUSH,STRAIGHT,Alice},              // flush beats straight
        {"AhJc","6h2d","Th3h4h5d5h",FLUSH,FLUSH,Alice},                 // highest flush wins
        {"7h6c","6h2d","AhKhQh9h8h",FLUSH,FLUSH,tie},                   // only five cards matter in a flush
        {"7h6h","5h2h","AhKhQh9h8h",FLUSH,FLUSH,tie},                   // only five cards matter in a flush
        {"7d6d","5h2h","7h7c6hTh8h",FULL_HOUSE,FLUSH,Alice},            // full house beats flush
        {"7d6d","6c6s","7h7c6h9h8h",FULL_HOUSE,FULL_HOUSE,Alice},       // with two full houses, higher trips win
        {"7d7s","6c6s","7h2c6h9h9s",FULL_HOUSE,FULL_HOUSE,Alice},       // with two full houses, higher trips win
        {"7d7s","6c6s","9c2c6h9h9s",FULL_HOUSE,FULL_HOUSE,Alice},       // if the trips match, higher pairs win
        {"AdKd","QcJs","9c6c6h9h9s",FULL_HOUSE,FULL_HOUSE,tie},         // there are no kickers in full houses
        {"AdKd","AcQs","AsAhQhQdKs",FULL_HOUSE,FULL_HOUSE,Alice},       // two trips don't matter
        {"2d2c","AcQs","AsAhQh2h2s",QUADS,FULL_HOUSE,Alice},            // quads beat a full house
        {"2d2c","3c3s","3d3hQh2h2s",QUADS,QUADS,Bob},                   // higher quads win
        {"Ad7c","Qc3s","2d2cQh2h2s",QUADS,QUADS,Alice},                 // quads + higher kicker wins
        {"AdKc","AcQs","2d2cQh2h2s",QUADS,QUADS,tie},                   // only one kicker matters with quads
        {"2d3d","AcAs","AdAh4d5d6d",STRAIGHT_FLUSH,QUADS,Alice},        // straight flush beats quads
        {"Ts8s","QsJs","2s3s4s5s6s",STRAIGHT_FLUSH,STRAIGHT_FLUSH,tie}, // kickers don't matter with straight flushes
        {"Ah5c","Tc2h","6c7c8c9cKh",STRAIGHT_FLUSH,STRAIGHT_FLUSH,Bob}, // highest straight flush wins
        {"AhJc","5c5s","2h3h4h5d5h",STRAIGHT_FLUSH,QUADS,Alice},        // aces can be low in straight flushes
        {"AhJc","6hKh","2h3h4h5h5d",STRAIGHT_FLUSH,STRAIGHT_FLUSH,Bob}, // the steel wheel is the lowest straight flush
        {"7d8h","7h2c","2h3h4h5h6h",STRAIGHT_FLUSH,STRAIGHT_FLUSH,Bob}, // higher straight flush beats higher flush and straight
    };
    size_t n = sizeof(tests)/sizeof(test_t);

    // Collect hands to score
    cards_t cards[2*n];
    for (size_t i = 0; i < sizeof(tests)/sizeof(test_t); i++) {
        const cards_t alice  = read_cards(tests[i].alice),
                      bob    = read_cards(tests[i].bob),
                      shared = read_cards(tests[i].shared);
        if (popcount(alice|bob|shared)!=9) {
            cout<<"test "<<tests[i].alice<<' '<<tests[i].bob<<' '<<tests[i].shared<<" has duplicated cards"<<endl;
            exit(1);
        }
        cards[2*i+0] = alice|shared;
        cards[2*i+1] = bob|shared;
    }

    // Score them
    score_t scores[2*n];
    score_hands_opencl(0,2*n,scores,cards);

    // Check results
    for (size_t i = 0; i < sizeof(tests)/sizeof(test_t); i++) {
        const score_t alice_score = scores[2*i+0],
                      bob_score   = scores[2*i+1],
                      alice_type  = alice_score&TYPE_MASK,
                      bob_type    = bob_score&TYPE_MASK;
        const char* result = alice_score>bob_score?Alice:alice_score<bob_score?Bob:tie;
        if (alice_type!=tests[i].alice_type || bob_type!=tests[i].bob_type || result!=tests[i].result) {
            cout<<"test "<<tests[i].alice<<' '<<tests[i].bob<<' '<<tests[i].shared
                <<": expected "<<show_type(tests[i].alice_type)<<' '<<show_type(tests[i].bob_type)<<' '<<tests[i].result
                <<", got "<<show_type(alice_type)<<' '<<show_type(bob_type)<<' '<<result<<endl;
            exit(1);
        }
    }
}

inline cards_t mostly_random_set(uint64_t r) {
    cards_t cards = 0;
    #define ADD(a) \
        int i##a = (r>>(6*a)&0x3f)%52; \
        cards_t b##a = (cards_t)1<<i##a; \
        cards |= cards&b##a?min_bit(~cards):b##a;
    ADD(0) ADD(1) ADD(2) ADD(3) ADD(4) ADD(5) ADD(6)
    assert(popcount(cards)==7);
    return cards;
}

void regression_test_score_hand(size_t multiple) {
    timer_t timer("test score hands");
    // Score a large number of hands
    const size_t m = multiple<<17, n = 1<<10;
    cout<<"score test: scoring "<<m*n<<" hands"<<endl;
    vector<uint64_t> hashes(m);
    hash_scores_opencl(0,m,&hashes[0]);

    // Merge hashes
    uint64_t merged = 0;
    for (uint64_t i = 0; i < m; i++)
        merged = hash2(merged,hashes[i]);

    const uint64_t expected = multiple==1 ?0x014580e94b28f5ce:
                              multiple==2 ?0xd1a032a5cb17fd93:
                              multiple==10?0xd4222a11207e32e9:0;
    if (merged!=expected) {
        if (expected) {
            cout<<"score test: expected 0x"<<std::hex<<expected<<", got 0x"<<merged<<std::dec<<endl;
            exit(1);
        } else
            cout<<"score test: expected value for n = "<<multiple<<std::hex<<" not known, got 0x"<<merged<<std::dec<<endl;
    } else
        cout<<"score test passed!"<<endl;
}

// List of all possible two card hands
vector<hand_t> hands;

void compute_hands() {
    for (int c0 = 0; c0 < 13; c0++) {
        hands.push_back(hand_t(c0,c0,0));
        for (int c1 = 0; c1 < c0; c1++)
            for (int s = 0; s < 2; s++)
                hands.push_back(hand_t(c0,c1,s));
    }
    assert(hands.size()==169);
}

vector<outcomes_t> compare_many_hands(const vector<hand_t>& pairs, bool verbose) {
    assert(pairs.size()%2==0);
    size_t n = pairs.size()/2;
    size_t next = 0, show = 0;
    vector<outcomes_t> outcomes(n);
    #pragma omp parallel num_threads(devices.size()) 
    {
        size_t device = omp_get_thread_num();
        for (;;) {
            // Grab next free job
            size_t job;
            #pragma omp critical 
            job = next++;
            if (job>=n) break;
            // Compute
            outcomes_t o = compare_hands(device,pairs[2*job],pairs[2*job+1]);
            // Store results and optionally print
            #pragma omp critical
            {
                outcomes[job] = o;
                while (show<n && outcomes[show].total()) {
                    if (verbose)
                        show_comparison(pairs[2*show],pairs[2*show+1],outcomes[show]);
                    else
                        cout<<(show?", ":"")<<pairs[2*show]<<" vs. "<<pairs[2*show+1]<<flush;
                    show++;
                }
            }
        }
    }
    return outcomes;
}

void regression_test_compare_hands(size_t n) {
    timer_t timer("test compare hands");
    cout<<"compare test: comparing "<<n<<" random pairs of hands, including at least one matched pair"<<endl;
    vector<hand_t> pairs;
    for (uint64_t i = 0; i <= n; i++) {
        hand_t alice =   hands[hash2(i,0)%hands.size()],
               bob   = i?hands[hash2(i,1)%hands.size()]:alice;
        pairs.push_back(alice);
        pairs.push_back(bob);
    }
    vector<outcomes_t> outcomes = compare_many_hands(pairs,false);
    uint64_t signature = 0;
    for (uint64_t i = 0; i <= n; i++) {
        outcomes_t o = outcomes[i];
        signature = hash2(signature,hash3(o.alice,o.bob,o.tie));
    }
    cout<<endl;
    const uint64_t expected = n==1 ?0xb034c27133337c71:
                              n==2 ?0x80e493c487b72627:
                              n==10?0x006c9a21c45a67b5:0;
    if (signature!=expected) {
        if (expected) {
            cout<<"compare test: expected 0x"<<std::hex<<expected<<", got 0x"<<signature<<std::dec<<endl;
            exit(1);
        } else
            cout<<"compare test: expected value for n = "<<n<<std::hex<<" not known, got 0x"<<signature<<std::dec<<endl;
    } else
        cout<<"compare test passed!"<<endl;
}

void usage(const char* program) {
    cerr<<"usage: "<<program<<" [options...] <command> [args...]\n"
          "options:\n"
          "  -a, --all      use all available OpenCL devices (GPUs and CPUs)\n"
          "  -g, --gpu      use only GPUs\n"
          "  -c, --cpu      use only CPUs\n"
          "  -n, --nop      count the number of hands we'd evaluate, but don't actually compute\n"
          "commands:\n"
          "  hands          print list of two card hold'em hands\n"
          "  test [n]       run some moderately expensive regression tests, with an optional size parameter\n"
          "  some [n]       compute win/loss/tie probabilities for some random pairs of hands\n"
          "  all            compute win/loss/tie probabilities for all pairs of hands\n"
        <<flush;
}

} // unnamed namespace

int main(int argc, char** argv) {
    timer_t timer("all");
    const char* program = argv[0];
    int device_types = CL_DEVICE_TYPE_ALL;

    const option options[] = {
        {"cpu",no_argument,0,'c'},
        {"gpu",no_argument,0,'g'},
        {"all",no_argument,0,'a'},
        {"nop",no_argument,0,'n'},
        {0,0,0,0}};
    int ch;
    while ((ch = getopt_long(argc,argv,"cgan",options,0)) != -1)
         switch (ch) {
             case 'c': device_types = CL_DEVICE_TYPE_CPU; break;
             case 'g': device_types = CL_DEVICE_TYPE_GPU; break;
             case 'a': device_types = CL_DEVICE_TYPE_ALL; break;
             case 'n': do_nothing = true; break;
             default: usage(program); return 1;
    }
    argc -= optind;
    argv += optind;

    if (argc<1) {
        usage(program);
        cerr<<"command expected"<<endl;
        return 1;
    }
    string cmd = argv[0];

    // Initialize
    {
        timer_t timer("initialize");
        compute_five_subsets();
        compute_hands();
        initialize_opencl(device_types,true);
    }

    // Run a few tests
    test_score_hand();

    // Print hands
    if (cmd=="hands") {
        cout<<"hands =";
        for (size_t i = 0; i < hands.size(); i++)
            cout<<' '<<hands[i];
        cout<<endl;
    }

    // Run more expensive tests
    else if (cmd=="test") {
        size_t m = argc<2?1:atoi(argv[1]);
        regression_test_compare_hands(m);
        regression_test_score_hand(m);
    }

    // Compute equities for some (mostly random) pairs of hands
    else if (cmd=="some") {
        size_t n = argc<2?10:atoi(argv[1]);
        vector<hand_t> pairs;
        for (size_t r = 0; r < 2*n; r++)
            pairs.push_back(hands[hash(r)%hands.size()]);
        compare_many_hands(pairs,true);
    }

    // Compute all hand pair equities
    else if (cmd=="all") {
        vector<hand_t> pairs;
        for (size_t i = 0; i < hands.size(); i++)
            for (size_t j = 0; j <= i; j++) {
                pairs.push_back(hands[i]);
                pairs.push_back(hands[j]);
            }
        compare_many_hands(pairs,true);
    }

    // Didn't understand command
    else {
        usage(program);
        cerr<<"unknown command: "<<cmd<<endl;
        return 1;
    }

    if (total_comparisons)
        cerr<<"total comparisons = "<<total_comparisons<<endl;

    timer.close();
    timer_t::dump();
    return 0;
}
