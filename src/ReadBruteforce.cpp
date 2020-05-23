/*
 * Copyright 2020 Florent Bondoux
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"

#include "ReadBruteforce.h"
#include "ExpandCharset.h"
#include "utf_conv.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <memory>
#include <list>
#include <type_traits>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef HAVE_GETLINE
# include "getline.h"
#endif

namespace Maskgen {

/**
 * @brief Describe a charset and its constraints
 * 
 * @param T Either char or 8-bit charsets or uint32_t for unicode codepoints
 */
template<typename T>
struct ConstrainedCharset {
    Charset<T> m_charset;   /*!< the charset (must be fully expanded) */
    unsigned int m_min;     /*!< minimum number of occurrences */
    unsigned int m_max;     /*!< maximum number of occurrences */

    /**
     * @brief Construct a new charset with constraints
     * 
     * @param charset must be already expanded
     * @param min minimum number of occurrences
     * @param max maximum number of occurrences
     */
    ConstrainedCharset(DefaultCharset<T> &charset, unsigned int min, unsigned int max) :
        m_charset(charset.cset.data(), charset.cset.size()), m_min(min), m_max(max) {}
};

template<typename T>
static void enumerateMasks_rec_inner(const std::vector<std::pair<const ConstrainedCharset<T> *, unsigned int>> &constraints,
                               std::vector<unsigned int> &counts, std::vector<const ConstrainedCharset<T> *> &mask,
                               unsigned int target_len, MaskList<T> &ml);

/**
 * @brief 2nd stage of the mask generation, enumerate over the masks from a valid constraints set
 * 
 * @param T Either char or 8-bit charsets or uint32_t for unicode codepoints
 * @param constraints charsets and number of occurrences in the word
 * @param target_len length of the words
 * @param ml output
 */
template<typename T>
static void enumerateMasks_rec(
        const std::vector<std::pair<const ConstrainedCharset<T> *, unsigned int>> &constraints,
        unsigned int target_len, MaskList<T> &ml)
{
    // how many times a charset must be used
    std::vector<unsigned int> counts(constraints.size());
    // the progression of the mask from left to right
    std::vector<const Charset<T> *> mask;
    mask.reserve(target_len);
    for (size_t i = 0; i < constraints.size(); i++) {
        counts[i] = constraints[i].second;
    }
    
    enumerateMasks_rec_inner(constraints, counts, mask, target_len, ml);
}

/**
 * Recursive implementation of enumerateMasks_rec<T>
 * The recursion depth is the word's length + 1
 */
template<typename T>
static void enumerateMasks_rec_inner(const std::vector<std::pair<const ConstrainedCharset<T> *, unsigned int>> &constraints,
                               std::vector<unsigned int> &counts, std::vector<const Charset<T> *> &mask,
                               unsigned int target_len, MaskList<T> &ml)
{
    if (mask.size() == target_len) {
        // this mask is done cooking
        ml.emplaceMask(mask);
    }
    else {
        for (size_t i = 0; i < counts.size(); i++) {
            if (counts[i] > 0) {
                counts[i]--;
                mask.push_back(&constraints[i].first->m_charset);
                enumerateMasks_rec_inner(constraints, counts, mask, target_len, ml);
                mask.pop_back();
                counts[i]++;
            }
        }
    }
}


/**
 * @brief Create the masks from the given charsets and constraints
 * 
 * The first stage of the mask generation is to deduce some valid reduced constraints.
 * For example, if we have:
 *   ?d: 4 to 6
 *   ?l: 0 to 1
 *   length: 6
 *
 * the valid constraints are:
 *   ?d*5, ?l*1
 *   ?d*6, ?l*0
 *
 * For each of those valid constraints, call \a enumerateMasks_rec to get the associated masks
 * 
 * @param T Either char or 8-bit charsets or uint32_t for unicode codepoints
 * @param constraints input data
 * @param target_len target word length
 * @param ml output
 */
template<typename T>
static void maskListFromConstraints(
        const std::list<ConstrainedCharset<T>> &constraints,
        unsigned int target_len, MaskList<T> &ml)
{
    // for each charset, number of occurrences
    // initialize this with the minimum number for each charsets
    std::vector<std::pair<const ConstrainedCharset<T> *, unsigned int>> counts;
    counts.reserve(constraints.size());
    unsigned int current_len = 0; // our current word length
    for (const auto &c : constraints) {
        counts.emplace_back(&c, c.m_min);
        current_len += c.m_min;
    }

    // now we iterates through the possible combinations keeping only those of required length
    while (true) {
        if (current_len < target_len) {
            // skip a few invalid combinations
            auto it = counts.begin();
            unsigned int diff = std::min(target_len - current_len, it->first->m_max - it->second);
            it->second += diff;
            current_len += diff;
        }

        if (current_len == target_len) {
            // this one is valid!
            enumerateMasks_rec(counts, target_len, ml);
        }

        bool carry = true;
        for (auto it = counts.begin(); it != counts.end() && carry; it++) {
            // now increment
            it->second++;
            current_len++;
            if (it->second > it->first->m_max || current_len > target_len) {
                // and also skip some other invalid combinations
                current_len -= it->second;
                it->second = it->first->m_min;
                current_len += it->second;
                carry = true;
            }
            else {
                carry = false;
            }
        }
        if (carry) {
            break;
        }
    }
}

struct Helper8bits
{
    static bool readCharset(const char *input, size_t, std::vector<char> &cset)
    {
        while (*input != 0) {
            cset.push_back(*input);
            input++;
        }
        return true;
    }
};

struct HelperUnicode
{
    static bool readCharset(const char *input, size_t input_len, std::vector<uint32_t> &cset)
    {
        size_t conv_consumed = 0, conv_written = 0;
        if (UTF::decode_utf8(input, input_len, std::back_inserter(cset), &conv_consumed, &conv_written) != UTF::RetCode::OK) {
            return false;
        }
        return true;
    }
};

/**
 * @brief Read a bruteforce description file
 * 
 * The format of the input file is :
 * <word width>
 * <min1> <max1> <charset1>
 * <min2> <max2> <charset2>
 *
 * no comment, no escape char, empty lines are allowed, no extra space
 *
 * For the unicode version, the charset is expected to be in UTF-8
 * 
 * @param T Either char or 8-bit charsets or uint32_t for unicode codepoints
 * @param spec filename
 * @param charsets defined charsets
 * @param ml output
 * @return true if no error
 */
template<typename T>
bool readBruteforce(const char *spec, const CharsetMap<T> &charsets, MaskList<T> &ml)
{
    static_assert(std::is_same<T, char>::value || std::is_same<T, uint32_t>::value, "readBruteforce requires char or uint32_t as template parameter");
    typedef typename std::conditional<std::is_same<T, char>::value, Helper8bits, HelperUnicode>::type Helper;

#if defined(__WINDOWS__) || defined(__CYGWIN__)
    int fd = open(spec, O_RDONLY | O_BINARY);
#else
    int fd = open(spec, O_RDONLY);
#endif
    if (fd < 0) {
        fprintf(stderr, "Error: can't open file '%s': %m\n", spec);
        return false;
    }

    FILE *f = fdopen(fd, "rb");
    char *line = NULL;
    size_t line_size = 0;
    ssize_t r;
    unsigned int line_number = 0;

    bool got_mask_len = false;
    unsigned int mask_len = 0;

    // the first objective is to build this list describing the constraints read from the file
    std::list<ConstrainedCharset<T>> constrained_charsets;

    while ((r = getline(&line, &line_size, f))!= -1) {
        line_number++;
        if (r >= 2 && line[r-1] == '\n' && line[r-2] == '\r') {
            line[r-2] = '\0';
            r -= 2;
        }
        else if (r >= 1 && line[r-1] == '\n') {
            line[r-1] = '\0';
            r -= 1;
        }
        if (r == 0) {
            continue;
        }

        if (!got_mask_len) {
            if (sscanf(line, "%u", &mask_len) != 1) {
                fprintf(stderr, "Error while reading the width from '%s' at line '%u'\n", spec, line_number);
                fclose(f);
                free(line);
                return false;
            }
            got_mask_len = true;
        }
        else {
            int consumed = 0;
            unsigned int min_len = 0, max_len = 0;
            if (sscanf(line, "%u %u %n", &min_len, &max_len, &consumed) != 2) {
                fprintf(stderr, "Error while reading the charset constraints from '%s' at line '%u'\n", spec, line_number);
                fclose(f);
                free(line);
                return false;
            }

            DefaultCharset<T> new_charset;
            new_charset.final = false;

            if (!Helper::readCharset(line + consumed, r - consumed, new_charset.cset)) {
                fprintf(stderr, "Error: the charset at line '%u' is invalid\n", line_number);
                fclose(f);
                free(line);
                return false;
            }

            if (new_charset.cset.size() == 0) {
                fprintf(stderr, "Error: the charset at line '%u' is empty\n", line_number);
                fclose(f);
                free(line);
                return false;
            }

            // now to expand this charset, we need a name...
            // this charset is anonymous, so let's use a forbidden name for it
            // two charset names can't be used by the user: \0 and ?
            if (!expandCharset(charsets, new_charset, T('\0'))) {
                fprintf(stderr, "Error while expanding the charset from '%s' at line '%u'\n", spec, line_number);
                fclose(f);
                free(line);
                return false;
            }

            if (max_len > mask_len) {
                max_len = mask_len;
            }
            constrained_charsets.emplace_back(new_charset, min_len, max_len);
        }
    }

    if (constrained_charsets.size() == 0 || !got_mask_len) {
        fprintf(stderr, "Error, expected at least a word width and a charset in '%s'\n", spec);
        fclose(f);
        free(line);
        return false;
    }

    // now let's generate the masks
    maskListFromConstraints<T>(constrained_charsets, mask_len, ml);

    free(line);
    fclose(f);
    return true;
}

bool readBruteforceAscii(const char *spec, const CharsetMapAscii &charsets, MaskList<char> &ml)
{
    return readBruteforce<char>(spec, charsets, ml);
}

bool readBruteforceUtf8(const char *spec, const CharsetMapUnicode &charsets, MaskList<uint32_t> &ml)
{
    return readBruteforce<uint32_t>(spec, charsets, ml);
}

// Simon Tatham's style
//
// These macros are helpers to build pseudo coroutines (more like reentrant functions)
// The technique is proposed by Simon Tathams in "Coroutines in C" https://www.chiark.greenend.org.uk/~sgtatham/coroutines.html
// and is based on the old "Duff's device" loop optimisation
//
// The idea is to use a swicth statement as fancy label/goto to jump back at a saved position (juste ater a return statement).
// The state and variables must use a persistent storage.
// There are some limitations due to variable cross initialization but it's blazing-fast and kinda cute.
// Since there is no real stack, no context switching, etc. it's way faster than boost's coroutines, and still 3 to 4 times
// faster than C++20's coroutines as implemented by G++-10.1 (and they are really fast!)
#define crBegin switch(state) { case 0:
#define crReturn do { state=__LINE__; return true; case __LINE__:; } while (0);
#define crFinish } return false;

/**
 * @brief 2nd stage of the mask generation
 * 
 * A generator which yields the masks satisfying a set of reduced constraints (words's width and exact number of occurrences for each charsets).
 * Must be called by \a FirstStageGen<T>
 * 
 * Note that de \a counts parameter is passed by reference and is used by this generator
 * At the end of the generation, \a counts is back in is initial state.
 * 
 * This generator is recursive
 * 
 * @param T Either char or 8-bit charsets or uint32_t for unicode codepoints
 */
template<typename T>
class SecondStageGen {
    unsigned int state;
    struct {
        // charsets with current remaining number of occurrences
        // this reference is shared with the parent FirstStageGen and the recursive
        // instances of SecondStateGen
        std::vector<std::pair<const ConstrainedCharset<T> *, unsigned int>> &counts;
        // words's width
        unsigned int target_len;
        // current mask in cooking shared with the recursive instances of SecondStateGen
        // the ptr is allocated by the main constructor and copied by the 2nd
        std::shared_ptr<std::list<const ConstrainedCharset<T> *>> mask;
    } params;
    struct { // generator's pseudo stack
        size_t i;
        SecondStageGen<T> *ngen;
    } vars;

public:
    SecondStageGen(
            std::vector<std::pair<const ConstrainedCharset<T> *, unsigned int>> &counts,
            unsigned int target_len) :
            state(0),
            params {counts,
                    target_len,
                    std::make_shared<typename decltype(params.mask)::element_type>()
                    },
            vars() {}

    // copy a generator and reset it's state
    SecondStageGen(const SecondStageGen &o) : state(0), params(o.params), vars() {}

    bool operator()(std::list<const ConstrainedCharset<T> *> &mask_out) {
        crBegin
        if (params.mask->size() == params.target_len) {
            // done cooking!
            mask_out = *params.mask;
            crReturn
        }
        else {
            for (vars.i = 0; vars.i < params.counts.size(); vars.i++) {
                if (params.counts[vars.i].second > 0) {
                    params.counts[vars.i].second--;
                    params.mask->push_back(params.counts[vars.i].first);
                    vars.ngen = new SecondStageGen(*this);
                    while ((*vars.ngen)(mask_out)) {
                        // climb back up
                        crReturn
                    }
                    params.counts[vars.i].second++;
                    params.mask->pop_back();
                    delete vars.ngen;
                }
            }
        }
        crFinish
    }
};

/**
 * @brief Create the masks from the given charsets and constraints
 * 
 * This generator yields he masks satisfying the set of constraints
 * 
 * The first stage of the mask generation is to deduce some valid reduced constraints.
 * For example, if we have:
 *   ?d: 4 to 6
 *   ?l: 0 to 1
 *   length: 6
 *
 * the valid constraints are:
 *   ?d*5, ?l*1
 *   ?d*6, ?l*0
 *
 * For each of those valid constraints, use a \a SecondStageGen to get the associated masks
 * 
 * @param T Either char or 8-bit charsets or uint32_t for unicode codepoints
 */
template<typename T>
class FirstStageGen {
    unsigned int state;
    struct {
        const std::vector<ConstrainedCharset<T>> &constraints; // constained charsets
        unsigned int target_len; // word's width
    } params;
    struct {
        std::vector<std::pair<const ConstrainedCharset<T> *, unsigned int>> counts; // number of occurrences for each charsets
        unsigned int current_len; // current word's width
        SecondStageGen<T> *gen2;
    } vars;

public:
    FirstStageGen(const std::vector<ConstrainedCharset<T>> &constraints, unsigned int target_len):
        state(0), params {constraints, target_len}, vars() {}

    bool operator()(std::list<const ConstrainedCharset<T> *> &mask_out) {
        crBegin

        // initialize the number of occurrences with the minimum allowed for each charsets
        vars.counts.resize(params.constraints.size());
        vars.current_len = 0;
        for (size_t i = 0; i < params.constraints.size(); i++) {
            vars.counts[i].first = &params.constraints[i];
            vars.counts[i].second = params.constraints[i].m_min;
            vars.current_len += params.constraints[i].m_min;
        }

        while (true) {
            // skip a few invalid combinations
            if (vars.current_len < params.target_len) {
                auto it = vars.counts.begin();
                unsigned int diff = std::min(params.target_len - vars.current_len, it->first->m_max - it->second);
                it->second += diff;
                vars.current_len += diff;
            }

            // this combination is valid
            if (vars.current_len == params.target_len) {
                // use the 2nd generator to generate its masks
                // and yield them
                vars.gen2 = new SecondStageGen<T>(vars.counts, params.target_len);
                while ((*vars.gen2)(mask_out)) {
                    crReturn
                }
                delete vars.gen2;
            }

            // increment the combination
            bool carry = true;
            for (auto it = vars.counts.begin(); it != vars.counts.end() && carry; it++) {
                it->second++;
                vars.current_len++;
                if (it->second > it->first->m_max || vars.current_len > params.target_len) {
                    // also skip some other invalid combinations
                    vars.current_len -= it->second;
                    it->second = it->first->m_min;
                    vars.current_len += it->second;
                    carry = true;
                }
                else {
                    carry = false;
                }
            }
            if (carry) {
                break;
            }
        }

        crFinish
    }
};

#undef crBegin
#undef crFinish
#undef crReturn

/**
 * @brief A mask generator for bruteforce files
 * 
 * It's a wrapper over \a FirstStageGen<T>
 * 
 * @param T Either char or 8-bit charsets or uint32_t for unicode codepoints
 */
template<typename T>
class BruteforceGenerator : public MaskGenerator<T>
{
    const std::vector<ConstrainedCharset<T>> m_constraints; /*!< input data */
    unsigned int m_target_len; /*!< bruteforce width */
    FirstStageGen<T> *m_gen; /*!< The actual generator */
    bool m_done; /*!< flag */
    
public:
    BruteforceGenerator(const std::vector<ConstrainedCharset<T>> &constraints, unsigned int target_len) :
    m_constraints(constraints), m_target_len(target_len),
    m_gen(new FirstStageGen<T>(m_constraints, m_target_len)),
    m_done(false)
    {}
    ~BruteforceGenerator() {
        delete m_gen;
    }
    
    bool operator()(Maskgen::Mask<T> &mask) {
        if (m_done) {
            return false;
        }
        std::list<const ConstrainedCharset<T> *> mask_l;
        if ((*m_gen)(mask_l)) {
            mask.clear();
            for (auto &c: mask_l) {
                mask.push_charset_right(c->m_charset);
            }
            return true;
        }
        else {
            m_done = true;
            return false;
        }
    }
    
    void reset() {
        delete m_gen;
        m_gen = new FirstStageGen<T>(m_constraints, m_target_len);
        m_done = false;
    }
    
    bool good() {
        return true; // we don't do errors here. 
    }
};

template<typename T>
MaskGenerator<T> *readBruteforce__(const char *spec, const CharsetMap<T> &charsets)
{
    static_assert(std::is_same<T, char>::value || std::is_same<T, uint32_t>::value, "readBruteforce requires char or uint32_t as template parameter");
    typedef typename std::conditional<std::is_same<T, char>::value, Helper8bits, HelperUnicode>::type Helper;

#if defined(__WINDOWS__) || defined(__CYGWIN__)
    int fd = open(spec, O_RDONLY | O_BINARY);
#else
    int fd = open(spec, O_RDONLY);
#endif
    if (fd < 0) {
        fprintf(stderr, "Error: can't open file '%s': %m\n", spec);
        return NULL;
    }

    FILE *f = fdopen(fd, "rb");
    char *line = NULL;
    size_t line_size = 0;
    ssize_t r;
    unsigned int line_number = 0;

    bool got_mask_len = false;
    unsigned int mask_len = 0;

    // the first objective is to build this list describing the constraints read from the file
    std::list<ConstrainedCharset<T>> constrained_charsets;

    while ((r = getline(&line, &line_size, f))!= -1) {
        line_number++;
        if (r >= 2 && line[r-1] == '\n' && line[r-2] == '\r') {
            line[r-2] = '\0';
            r -= 2;
        }
        else if (r >= 1 && line[r-1] == '\n') {
            line[r-1] = '\0';
            r -= 1;
        }
        if (r == 0) {
            continue;
        }

        if (!got_mask_len) {
            if (sscanf(line, "%u", &mask_len) != 1) {
                fprintf(stderr, "Error while reading the width from '%s' at line '%u'\n", spec, line_number);
                fclose(f);
                free(line);
                return NULL;
            }
            got_mask_len = true;
        }
        else {
            int consumed = 0;
            unsigned int min_len = 0, max_len = 0;
            if (sscanf(line, "%u %u %n", &min_len, &max_len, &consumed) != 2) {
                fprintf(stderr, "Error while reading the charset constraints from '%s' at line '%u'\n", spec, line_number);
                fclose(f);
                free(line);
                return NULL;
            }

            DefaultCharset<T> new_charset;
            new_charset.final = false;

            if (!Helper::readCharset(line + consumed, r - consumed, new_charset.cset)) {
                fprintf(stderr, "Error: the charset at line '%u' is invalid\n", line_number);
                fclose(f);
                free(line);
                return NULL;
            }

            if (new_charset.cset.size() == 0) {
                fprintf(stderr, "Error: the charset at line '%u' is empty\n", line_number);
                fclose(f);
                free(line);
                return NULL;
            }

            // now to expand this charset, we need a name...
            // this charset is anonymous, so let's use a forbidden name for it
            // two charset names can't be used by the user: \0 and ?
            if (!expandCharset(charsets, new_charset, T('\0'))) {
                fprintf(stderr, "Error while expanding the charset from '%s' at line '%u'\n", spec, line_number);
                fclose(f);
                free(line);
                return NULL;
            }

            if (max_len > mask_len) {
                max_len = mask_len;
            }
            constrained_charsets.emplace_back(new_charset, min_len, max_len);
        }
    }
    
    fclose(f);
    free(line);

    if (constrained_charsets.size() == 0 || !got_mask_len) {
        fprintf(stderr, "Error, expected at least a word width and a charset in '%s'\n", spec);
        return NULL;
    }

    return new BruteforceGenerator<T>({constrained_charsets.begin(), constrained_charsets.end()}, mask_len);
}

MaskGenerator<char> *readBruteforceAscii__(const char *spec, const CharsetMapAscii &charsets)
{
    return readBruteforce__<char>(spec, charsets);
}

MaskGenerator<uint32_t> *readBruteforceUtf8__(const char *spec, const CharsetMapUnicode &charsets)
{
    return readBruteforce__<uint32_t>(spec, charsets);
}

}
