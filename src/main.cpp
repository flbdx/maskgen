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

#include <cstdio>
#include <cstring>
#include <cinttypes>

#include <string>
#include <map>

#include <getopt.h>

#include "ReadMasks.h"
#include "utf_conv.h"

using namespace Maskgen;

static void usage() {
    const char *help_string = 
    "Usage: maskgen [OPTIONS] (mask|maskfile)\n"
    "Generate words based on templates (masks) describing each position's charset\n"
    "\n"
    " Behavior:\n"
    "  -u, --unicode                Allow UTF-8 characters in the charsets\n"
    "                               Without this option, the charsets can only\n"
    "                               contain 8-bit (ASCII compatible) values\n"
    "                               This option slows down the generation and\n"
    "                               disables the '?b' built-in charset"
    "\n"
    " Range:\n"
    "  -j, --job=J/N                Devide the generation in N equal parts and\n"
    "                               generate the 'J'th part (counting from 1)\n"
    "  -b, --begin=N                Start the generation at the Nth word\n"
    "                               counting from 0\n"
    "  -e, --end=N                  Stop after the Nth word counting from 0\n"
    "\n"
    " Output control:\n"
    "  -o, --output=FILE            Write the words into FILE\n"
    "  -z, --zero                   Use the null character as a word delimiter\n"
    "                               instead of the newline character\n"
    "  -n, --no-delim               Don't use a word delimiter\n"
    "  -s, --size                   Show the number of words that will be\n"
    "                               generated and exit\n"
    "  -h, --help                   Show this help message and exit\n"
    "\n"
    " Charsets:\n"
    "  A charset is a named variable describing a list of characters. Unless\n"
    "  the --unicode option is used, only 8-bit characters are allowed.\n"
    "  The name of a charset is a single 8-bit character. It is refered using\n"
    "  '?' followed by its name (example : ?d). A charset definition can refer\n"
    "  to other named charsets.\n"
    "\n"
    "  Built-in charsets:\n"
    "   ?l = abcdefghijklmnopqrstuvwxyz\n"
    "   ?u = ABCDEFGHIJKLMNOPQRSTUVWXYZ\n"
    "   ?d = 0123456789\n"
    "   ?s =  !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\n"
    "   ?a = ?l?u?d?s\n"
    "   ?h = 0123456789abcdef\n"
    "   ?H = 0123456789ABCDEF\n"
    "   ?n = \\n (new line)\n"
    "   ?r = \\r (carriage ret)\n"
    "   ?b = 0x00 - 0xFF (only without --unicode)\n"
    "\n"
    "  Custom charsets:\n"
    "   Custom named charsets are defined either inline or by reading a file.\n"
    "   To include a single '?' in a charset, escape it with another '?' (?\?).\n"
    "   Pay attention to trailing newline when reading a file or to the shell\n"
    "   expansion for inline definitions ('?' or '*' chars...)\n"
    "\n"
    "   -1, --custom-charset1=CS    Define the charsets named '1', '2', '3' or\n"
    "   -2, --custom-charset2=CS    '4'. The argument is either the content of\n"
    "   -3, --custom-charset3=CS    the charset or a file to read.\n"
    "   -4, --custom-charset4=CS\n"
    "\n"
    "   -c, --charset=K:CS          Define a charset named 'K' with the content\n"
    "                               'CS'. 'K' must be a single 8-bit char.\n"
    "\n"
    " Masks:\n"
    "  Masks are templates defining wich characters are allowed for each\n"
    "  positions. Masks are single line strings build by concatenating\n"
    "  for each positions either: \n"
    "  - a static character\n"
    "  - a charset reference indicated by a '?' followed by the charset\n"
    "    name\n"
    "  For example, '@?u?l?l?l?d@' would generate the words from\n"
    "  '@Aaaa0@' to '@Zzzz9@'\n"
    "\n"
    "  The mask argument is either a single mask definition or a file\n"
    "  containing a list of masks definitions.\n"
    "\n"
    "  Mask files can also embed charset definitions. The general syntax for\n"
    "  a single line is:\n"
    "   [?1,][?2,]...[?9,]mask\n"
    "  where the placeholders are as follows:\n"
    "   [?1] the named custom charset '1' (override --custom-charset1 or -1)\n"
    "        will be set to this value, optional\n"
    "   [?2] the named custom charset '2' (override --custom-charset2 or -2)\n"
    "        will be set to this value, optional\n"
    "   ...\n"
    "   [?9] the named custom charset '9'  will be set to this value, optional\n"
    "   [mask] the mask which may refer to the previously defined charsets\n"
    "\n"
    "  The characters ',' and '?' can be escaped by writting '?,' or '?\?'.\n"
    "\n"
    ;
    
    printf("%s", help_string);
}

struct Options {
    bool m_unicode;
    uint64_t m_job_number;
    uint64_t m_job_total;
    bool m_job_set;
    uint64_t m_start_word;
    bool m_start_word_set;
    uint64_t m_end_word;
    bool m_end_word_set;
    std::string m_output_file;
    bool m_zero_delim;
    bool m_no_delim;
    bool m_print_size;
    std::map<char, std::string> m_charsets_defs;
    
    Options() :
    m_unicode(false)
    , m_job_number(), m_job_total(), m_job_set(false)
    , m_start_word(), m_start_word_set(false), m_end_word(), m_end_word_set(false)
    , m_output_file()
    , m_zero_delim(false), m_no_delim(false)
    , m_print_size(false)
    , m_charsets_defs()
    {}
};

class Print8bit {
public:
    inline void print(const char *buffer, size_t len, FILE *out) {
        fwrite(buffer, 1, len, out);
    }
};

class PrintUnicode {
    char *conv_buffer;
    size_t conv_buffer_size;
    
public:
    PrintUnicode() : conv_buffer(NULL), conv_buffer_size(0) {}
    ~PrintUnicode() {free(conv_buffer);};
    inline void print(const uint32_t *buffer, size_t len, FILE *out) {
        size_t consumed = 0, written = 0;
        if (UTF::encode_utf8(buffer, len, &conv_buffer, &conv_buffer_size, &consumed, &written) == UTF::RetCode::OK) {
            fwrite(conv_buffer, 1, written, out);
        }
        else {
            fprintf(stderr, "Error: could not encode the generated words into UTF-8\n");
            exit(1);
        }
    }
};

template<typename T,
void (initDefautCharsets(CharsetMap<T> &)),
bool (readCharset(const char *, std::vector<T> &)),
std::vector<T> (expandCharset)(const std::vector<T> &, const CharsetMap<T> &, T charset_name),
bool readMaskList(const char *spec, const CharsetMap<T> &, MaskList<T> &ml),
typename Printer>
int work(const struct Options &options, const char *mask_arg) {
    CharsetMap<T> charsets;
    initDefautCharsets(charsets);
    
    for (auto p : options.m_charsets_defs) {
        std::vector<T> charset;
        if (!readCharset(p.second.c_str(), charset)) {
            fprintf(stderr, "Error while reading the charset '%c'\n", p.first);
            exit(1);
        }
        charsets[p.first] = DefaultCharset<T>(charset, false);
    }
    for (auto &p : charsets) {
        if (p.second.final) {
            continue;
        }
        p.second.cset = expandCharset(p.second.cset, charsets, p.first);
        if (p.second.cset.empty()) {
            fprintf(stderr, "Error while expanding the charset '%c' (undefined charset ?)\n", p.first);
            return 1;
        }
        p.second.final = true;
    }
    
    
    MaskList<T> ml;
    if (!readMaskList(mask_arg, charsets, ml)) {
        fprintf(stderr, "Error while reading the mask definition '%s'\n", mask_arg);
        exit(1);
    }
    
    uint64_t ml_len = ml.getLen();
    
    if (options.m_print_size) {
        printf("%" PRIu64 "\n", ml_len);
        return 0;
    }
    
    uint64_t start_idx = 0;
    uint64_t end_idx = ml_len;
    
    if (options.m_job_set) {
        uint64_t q = ml_len / options.m_job_total;
        uint64_t r = ml_len - q * options.m_job_total;
        
        uint64_t todo = q;
        start_idx = q * (options.m_job_number - 1);
        if (r != 0) {
            start_idx += std::min((uint64_t) options.m_job_number - 1, r);
            todo += options.m_job_number <= r ? 1 : 0;
        }
        end_idx = start_idx + todo;
    }
    else {
        if (options.m_start_word_set) {
            start_idx = options.m_start_word;
        }
        if (options.m_end_word_set) {
            end_idx = options.m_end_word + 1;
        }
        
        if (end_idx - 1 < start_idx || end_idx > ml_len) {
            fprintf(stderr, "Error: the last word number is not valid\n");
            return 1;
        }
    }
    
    Printer printer;
    std::vector<T> buffer(BUFSIZ);
    T * buffer_p = buffer.data();
    const T * buffer_end = buffer.data() + buffer.size();
    std::vector<T> word(ml.getMaxWidth() + 1);
    if (word.size() > buffer.size()) {
        fprintf(stderr, "Error: do you reallly intend to generate words of length over %zu ?\n", buffer.size());
        exit(1);
    }
    
    // at last create the output file if needed now that we're not supposed to fail anymore
    FILE *outfile = stdout;
    if (!options.m_output_file.empty()) {
        outfile = fopen(options.m_output_file.c_str(), "wb");
        if (outfile == NULL) {
            fprintf(stderr, "Error: can't open the output file : %m\n");
            return 1;
        }
    }
    
    
    T delim = options.m_zero_delim ? '\0' : '\n';
    int delim_width = options.m_no_delim ? 0 : 1;
    uint64_t todo = end_idx - start_idx;
    ml.setPosition(start_idx);
    
    size_t w;
    for (uint64_t i = 0; i < todo; i++) {
        ml.getNext(word.data(), &w);
        word[w] = delim;
        if (w + delim_width > size_t(buffer_end - buffer_p)) {
            printer.print(buffer.data(), buffer_p - buffer.data(), outfile);
            buffer_p = buffer.data();
        }
        
        memcpy(buffer_p, word.data(), sizeof(T) * (w + delim_width));
        buffer_p += w + delim_width;
    }
    
    printer.print(buffer.data(), buffer_p - buffer.data(), outfile);
    if (outfile != stdout) {
        fclose(outfile);
    }
    return 0;
}

int main(int argc, char **argv)
{
    Options options;
    
    struct option longopts[] = {
        {"unicode", no_argument, NULL, 'u'},
        {"job", required_argument, NULL, 'j'},
        {"begin", required_argument, NULL, 'b'},
        {"end", required_argument, NULL, 'e'},
        {"output", required_argument, NULL, 'o'},
        {"zero", no_argument, NULL, 'z'},
        {"no-delim", no_argument, NULL, 'n'},
        {"size", no_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {"custom-charset1", required_argument, NULL, '1'},
        {"custom-charset2", required_argument, NULL, '2'},
        {"custom-charset3", required_argument, NULL, '3'},
        {"custom-charset4", required_argument, NULL, '4'},
        {"charset", required_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}
    };
    const char *shortopt = "uj:b:e:o:znsh1:2:3:4:c:";
    
    int opt;
    while ((opt = getopt_long(argc, argv, shortopt, longopts, 0)) >= 0) {
        switch (opt) {
            case 'u':
                options.m_unicode = true;
                break;
            case 'j':
            {
                int r = sscanf(optarg, "%" PRIu64 "/%" PRIu64, &options.m_job_number, &options.m_job_total);
                if (r != 2 || options.m_job_number > options.m_job_total || options.m_job_number == 0) {
                    fprintf(stderr, "Error: wrong job number specification (%s)\n", optarg);
                    exit(1);
                }
                options.m_job_set = true;
            }
                break;
            case 'b':
            {
                int r = sscanf(optarg, "%" PRIu64, &options.m_start_word);
                if (r != 1) {
                    fprintf(stderr, "Error: wrong starting word number specification (%s)\n", optarg);
                    exit(1);
                }
                options.m_start_word_set = true;
            }
                break;
            case 'e':
            {
                int r = sscanf(optarg, "%" PRIu64, &options.m_end_word);
                if (r != 1) {
                    fprintf(stderr, "Error: wrong last word number specification (%s)\n", optarg);
                    exit(1);
                }
                options.m_end_word_set = true;
            }
                break;
            case 'o':
                options.m_output_file = std::string(optarg);
                break;
            case 'z':
                options.m_zero_delim = true;
                break;
            case 'n':
                options.m_no_delim = true;
                break;
            case 's':
                options.m_print_size = true;
                break;
            case 'h':
                usage();
                exit(0);
            case '1':
            case '2':
            case '3':
            case '4':
                options.m_charsets_defs[opt] = std::string(optarg);
                break;
            case 'c':
            {
                size_t optlen = strlen(optarg);
                if (optlen < 3 || optarg[1] != ':') {
                    fprintf(stderr, "Error: wrong charset specification (%s)\n", optarg);
                    exit(1);
                }
                options.m_charsets_defs[optarg[0]] = std::string(optarg + 2);
            }
                break;
            default:
                usage();
                exit(1);
        }
    }
    
    argc -= optind;
    argv += optind;
    
    if (argc != 1) {
        fprintf(stderr, "Error: wrong number of arguments\n");
        exit(1);
    }
    
    const char *mask_arg = argv[0];
    
    if (!options.m_unicode) {
        int r = work<char, initDefaultCharsetsAscii, readCharsetAscii, expandCharsetAscii, readMaskListAscii, Print8bit>(options, mask_arg);
        return r;
    }
    else {
        int r = work<uint32_t, initDefaultCharsetsUnicode, readCharsetUtf8, expandCharsetUnicode, readMaskListUtf8, PrintUnicode>(options, mask_arg);
        return r;
    }

    return 0;
}
