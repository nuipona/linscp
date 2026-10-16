/*
 * sbcsgen.cpp - generate sbcsdat.c from sbcs.dat.
 *
 * This C++17 maintainer tool generates PuTTY's single-byte charset tables.
 * It also contains the two mapping-table import modes documented in sbcs.dat:
 *
 *   sbcsgen --unicode-map   (Unicode.org 0xNN 0xNNNN mapping tables on stdin)
 *   sbcsgen --czyborra-map  (Czyborra =NN U+NNNN mapping tables on stdin)
 *
 * Build/run from this directory with:
 *   c++ -std=c++17 -O2 sbcsgen.cpp -o /tmp/sbcsgen
 *   /tmp/sbcsgen -o sbcsdat.c
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

enum class Mode {
    Generate,
    Check,
    UnicodeMap,
    CzyborraMap,
    Help,
};

struct Options {
    Mode mode = Mode::Generate;
    std::filesystem::path input;
    std::filesystem::path output = "sbcsdat.c";
    std::filesystem::path expected = "sbcsdat.c";
    bool inputSpecified = false;
    bool outputSpecified = false;
    bool expectedSpecified = false;
};

struct Charset {
    std::string name;
    std::vector<int> values;
    std::array<int, 256> priorities{};
};

struct ReverseEntry {
    std::size_t byte;
    int codepoint;
    int priority;
};

constexpr std::size_t kTableSize = 256;

std::string usage(const char *program)
{
    std::ostringstream out;
    out << "usage: " << program << " [--input FILE] [-o OUTFILE]\n"
        << "       " << program
        << " --check [--input FILE] [--expected FILE]\n"
        << "       " << program << " --unicode-map\n"
        << "       " << program << " --czyborra-map\n";
    return out.str();
}

[[noreturn]] void usageError(const char *program, const std::string &message)
{
    throw std::runtime_error(message + "\n" + usage(program));
}

bool startsWith(const std::string &text, const std::string &prefix)
{
    return text.size() >= prefix.size() &&
        text.compare(0, prefix.size(), prefix) == 0;
}

void selectMode(Options &options, Mode mode, const char *program)
{
    if (options.mode != Mode::Generate && options.mode != mode)
        usageError(program, "Choose only one generator mode.");
    options.mode = mode;
}

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto takeValue = [&](const char *option) -> std::string {
            if (i + 1 >= argc)
                usageError(argv[0], std::string("Missing value for ") + option);
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            selectMode(options, Mode::Help, argv[0]);
        } else if (arg == "--check") {
            selectMode(options, Mode::Check, argv[0]);
        } else if (arg == "--unicode-map") {
            selectMode(options, Mode::UnicodeMap, argv[0]);
        } else if (arg == "--czyborra-map") {
            selectMode(options, Mode::CzyborraMap, argv[0]);
        } else if (arg == "-o" || arg == "--output") {
            options.output = takeValue(arg.c_str());
            options.outputSpecified = true;
        } else if (arg == "-i" || arg == "--input") {
            options.input = takeValue(arg.c_str());
            options.inputSpecified = true;
        } else if (arg == "--expected") {
            options.expected = takeValue(arg.c_str());
            options.expectedSpecified = true;
        } else if (startsWith(arg, "--output=")) {
            options.output = arg.substr(std::string("--output=").size());
            options.outputSpecified = true;
        } else if (startsWith(arg, "--input=")) {
            options.input = arg.substr(std::string("--input=").size());
            options.inputSpecified = true;
        } else if (startsWith(arg, "--expected=")) {
            options.expected = arg.substr(std::string("--expected=").size());
            options.expectedSpecified = true;
        } else {
            usageError(argv[0], "Unknown option: " + arg);
        }
    }

    if (options.mode == Mode::Check && options.outputSpecified)
        usageError(argv[0], "--output cannot be used with --check.");
    if (options.mode != Mode::Check && options.expectedSpecified)
        usageError(argv[0], "--expected is only valid with --check.");
    if ((options.mode == Mode::UnicodeMap ||
         options.mode == Mode::CzyborraMap) &&
        (options.inputSpecified || options.outputSpecified ||
         options.expectedSpecified))
        usageError(argv[0], "Mapping modes read stdin and write stdout.");

    return options;
}

std::filesystem::path findDefaultInput(const char *program)
{
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path programPath(program);
    if (programPath.has_parent_path())
        candidates.push_back(programPath.parent_path() / "sbcs.dat");
    candidates.emplace_back("sbcs.dat");
    candidates.emplace_back(
        std::filesystem::path(__FILE__).parent_path() / "sbcs.dat");

    for (const auto &candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
            return candidate;
    }

    throw std::runtime_error(
        "Cannot find sbcs.dat; run from the charset directory or pass --input.");
}

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open input file: " + path.string());
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad())
        throw std::runtime_error("Error reading file: " + path.string());
    return contents.str();
}

unsigned long parseHex(std::string token, const std::string &context)
{
    if (startsWith(token, "0x") || startsWith(token, "0X"))
        token.erase(0, 2);
    if (token.empty())
        throw std::runtime_error("Missing hexadecimal value in " + context);
    for (unsigned char character : token) {
        if (!std::isxdigit(character))
            throw std::runtime_error("Invalid hexadecimal value '" + token +
                                     "' in " + context);
    }

    std::size_t parsed = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(token, &parsed, 16);
    } catch (const std::exception &) {
        throw std::runtime_error("Invalid hexadecimal value '" + token +
                                 "' in " + context);
    }
    if (parsed != token.size())
        throw std::runtime_error("Invalid hexadecimal value '" + token +
                                 "' in " + context);
    return value;
}

int parseDecimal(const std::string &token, const std::string &context)
{
    std::size_t parsed = 0;
    int value = 0;
    try {
        value = std::stoi(token, &parsed, 10);
    } catch (const std::exception &) {
        throw std::runtime_error("Invalid priority '" + token + "' in " +
                                 context);
    }
    if (parsed != token.size())
        throw std::runtime_error("Invalid priority '" + token + "' in " +
                                 context);
    return value;
}

bool isDataLine(const std::string &line)
{
    if (line.empty())
        return false;
    const unsigned char first = static_cast<unsigned char>(line.front());
    return std::isdigit(first) ||
        (first >= 'a' && first <= 'f') ||
        (first >= 'A' && first <= 'F') || first == 'X';
}

std::vector<Charset> parseCharsets(const std::filesystem::path &path)
{
    const std::string contents = readFile(path);
    std::istringstream lines(contents);
    const std::regex priorityPattern(R"(^sortpriority ([^-]*)-([^-]*) (.*)$)");
    std::vector<Charset> charsets;
    std::optional<Charset> current;
    std::string line;
    std::size_t lineNumber = 0;

    auto finishCharset = [&]() {
        if (!current)
            return;
        if (current->values.size() != kTableSize) {
            throw std::runtime_error(
                path.string() + ": charset " + current->name + " has " +
                std::to_string(current->values.size()) +
                " values instead of 256");
        }
        charsets.push_back(std::move(*current));
        current.reset();
    };

    while (std::getline(lines, line)) {
        ++lineNumber;
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

        if (startsWith(line, "charset ")) {
            finishCharset();
            current.emplace();
            current->name = line.substr(std::string("charset ").size());
            current->values.clear();
            current->priorities.fill(0);
            continue;
        }

        if (startsWith(line, "sortpriority ")) {
            if (!current) {
                throw std::runtime_error(path.string() + ":" +
                    std::to_string(lineNumber) +
                    ": sortpriority appears outside a charset");
            }
            std::smatch match;
            if (!std::regex_match(line, match, priorityPattern)) {
                throw std::runtime_error(path.string() + ":" +
                    std::to_string(lineNumber) + ": malformed sortpriority");
            }
            const std::string location = path.string() + ":" +
                std::to_string(lineNumber);
            const unsigned long first = parseHex(match[1].str(), location);
            const unsigned long last = parseHex(match[2].str(), location);
            const int amount = parseDecimal(match[3].str(), location);
            if (first >= kTableSize || last >= kTableSize) {
                throw std::runtime_error(location +
                    ": sortpriority range is outside 00-FF");
            }
            for (unsigned long byte = first; byte <= last; ++byte)
                current->priorities[byte] += amount;
            continue;
        }

        if (!isDataLine(line))
            continue;
        if (!current) {
            throw std::runtime_error(path.string() + ":" +
                std::to_string(lineNumber) +
                ": character data appears outside a charset");
        }

        std::istringstream values(line);
        std::string token;
        while (values >> token) {
            const int value = token == "XXXX" ? -1 :
                static_cast<int>(parseHex(token, path.string() + ":" +
                                           std::to_string(lineNumber)));
            current->values.push_back(value);
            if (current->values.size() > kTableSize) {
                throw std::runtime_error(path.string() + ":" +
                    std::to_string(lineNumber) + ": charset " + current->name +
                    " has more than 256 values");
            }
        }
        if (current->values.size() == kTableSize)
            finishCharset();
    }

    if (current)
        finishCharset();
    if (charsets.empty())
        throw std::runtime_error("No complete charset tables found in " +
                                 path.string());
    return charsets;
}

void writeHex(std::ostream &output, unsigned long value, int width)
{
    output << "0x" << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(width) << value << std::dec << std::setfill(' ');
}

std::string generateSource(const std::filesystem::path &inputPath)
{
    const std::vector<Charset> charsets = parseCharsets(inputPath);
    std::ostringstream output;
    output << "/*\n"
           << " * sbcsdat.c - data definitions for single-byte character sets.\n"
           << " *\n"
           << " * Generated by sbcsgen.cpp from sbcs.dat.\n"
           << " * You should edit those files rather than editing this one.\n"
           << " */\n\n"
           << "#ifndef ENUM_CHARSETS\n\n"
           << "#include \"charset.h\"\n"
           << "#include \"internal.h\"\n\n";

    for (const Charset &charset : charsets) {
        output << "static const sbcs_data data_" << charset.name << " = {\n"
               << "    {\n";
        std::vector<ReverseEntry> reverse;
        reverse.reserve(kTableSize);
        for (std::size_t byte = 0; byte < kTableSize; ++byte) {
            if (byte != 0) {
                if (byte % 8 == 0)
                    output << ",\n    ";
                else
                    output << ", ";
            } else {
                output << "    ";
            }

            const int codepoint = charset.values[byte];
            if (codepoint < 0) {
                output << "ERROR ";
            } else {
                writeHex(output, static_cast<unsigned long>(codepoint), 4);
                reverse.push_back({byte, codepoint, charset.priorities[byte]});
            }
        }

        output << "\n    },\n    {\n";
        std::sort(reverse.begin(), reverse.end(),
            [](const ReverseEntry &left, const ReverseEntry &right) {
                if (left.codepoint != right.codepoint)
                    return left.codepoint < right.codepoint;
                if (left.priority != right.priority)
                    return left.priority > right.priority;
                return left.byte < right.byte;
            });

        std::vector<ReverseEntry> unique;
        unique.reserve(reverse.size());
        for (const ReverseEntry &entry : reverse) {
            if (unique.empty() || unique.back().codepoint != entry.codepoint)
                unique.push_back(entry);
        }

        for (std::size_t index = 0; index < unique.size(); ++index) {
            if (index != 0) {
                if (index % 8 == 0)
                    output << ",\n    ";
                else
                    output << ", ";
            } else {
                output << "    ";
            }
            writeHex(output, static_cast<unsigned long>(unique[index].byte), 2);
        }

        output << "\n    },\n    " << unique.size() << "\n};\n"
               << "const charset_spec charset_" << charset.name << " = {\n"
               << "    " << charset.name
               << ", read_sbcs, write_sbcs, &data_" << charset.name
               << "\n};\n\n";
    }

    output << "#else /* ENUM_CHARSETS */\n\n";
    for (const Charset &charset : charsets)
        output << "ENUM_CHARSET(" << charset.name << ")\n";
    output << "\n#endif /* ENUM_CHARSETS */\n";
    return output.str();
}

std::array<int, kTableSize> initialMap(bool czyborra)
{
    std::array<int, kTableSize> mapping;
    mapping.fill(-1);
    for (std::size_t byte = 0; byte < kTableSize; ++byte) {
        if (byte < 0x20 || byte == 0x7f ||
            (czyborra && byte >= 0x7f && byte < 0xa0))
            mapping[byte] = static_cast<int>(byte);
    }
    return mapping;
}

void writeMapping(const std::array<int, kTableSize> &mapping)
{
    for (std::size_t byte = 0; byte < kTableSize; ++byte) {
        if (mapping[byte] < 0)
            std::cout << "XXXX";
        else
            std::cout << std::hex << std::nouppercase << std::setfill('0')
                      << std::setw(4) << mapping[byte] << std::dec
                      << std::setfill(' ');
        std::cout << (byte % 16 == 15 ? '\n' : ' ');
    }
}

std::string readMappingInput()
{
    const std::istreambuf_iterator<char> begin(std::cin);
    const std::istreambuf_iterator<char> end;
    std::string contents(begin, end);
    if (std::cin.bad())
        throw std::runtime_error("Error reading mapping data from stdin");
    std::replace(contents.begin(), contents.end(), '\r', '\n');
    return contents;
}

int importUnicodeMap()
{
    std::array<int, kTableSize> mapping = initialMap(false);
    std::istringstream lines(readMappingInput());
    std::string line;
    std::size_t mappedEntries = 0;
    while (std::getline(lines, line)) {
        std::istringstream tokens(line);
        std::string byteToken;
        std::string codepointToken;
        if (!(tokens >> byteToken >> codepointToken) ||
            (!startsWith(byteToken, "0x") && !startsWith(byteToken, "0X")) ||
            (!startsWith(codepointToken, "0x") &&
             !startsWith(codepointToken, "0X")))
            continue;

        const unsigned long byte = parseHex(byteToken, "Unicode mapping input");
        const unsigned long codepoint = parseHex(codepointToken,
                                                  "Unicode mapping input");
        if (byte < kTableSize) {
            mapping[byte] = static_cast<int>(codepoint);
            ++mappedEntries;
        }
    }
    if (mappedEntries == 0)
        throw std::runtime_error("No Unicode mapping entries found on stdin");
    writeMapping(mapping);
    return 0;
}

int importCzyborraMap()
{
    std::array<int, kTableSize> mapping = initialMap(true);
    std::istringstream lines(readMappingInput());
    std::string line;
    std::size_t mappedEntries = 0;
    while (std::getline(lines, line)) {
        std::istringstream tokens(line);
        std::string byteToken;
        std::string codepointToken;
        if (!(tokens >> byteToken >> codepointToken) || byteToken.empty() ||
            byteToken.front() != '=' ||
            (!startsWith(codepointToken, "U+") &&
             !startsWith(codepointToken, "u+")))
            continue;

        byteToken.erase(0, 1);
        codepointToken.erase(0, 2);
        const unsigned long byte = parseHex(byteToken, "Czyborra mapping input");
        const unsigned long codepoint = parseHex(codepointToken,
                                                  "Czyborra mapping input");
        if (byte < kTableSize) {
            mapping[byte] = static_cast<int>(codepoint);
            ++mappedEntries;
        }
    }
    if (mappedEntries == 0)
        throw std::runtime_error("No Czyborra mapping entries found on stdin");
    writeMapping(mapping);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        Options options = parseOptions(argc, argv);
        if (options.mode == Mode::Help) {
            std::cout << usage(argv[0]);
            return 0;
        }
        if (options.mode == Mode::UnicodeMap)
            return importUnicodeMap();
        if (options.mode == Mode::CzyborraMap)
            return importCzyborraMap();

        if (!options.inputSpecified)
            options.input = findDefaultInput(argv[0]);
        const std::string generated = generateSource(options.input);

        if (options.mode == Mode::Check) {
            const std::string expected = readFile(options.expected);
            if (generated != expected) {
                std::cerr << "sbcsgen: generated charset data differs from "
                          << options.expected << "; regenerate it with sbcsgen.\n";
                return 1;
            }
            std::cout << "sbcsgen: generated charset data is up to date.\n";
            return 0;
        }

        std::ofstream output(options.output, std::ios::binary);
        if (!output)
            throw std::runtime_error("Cannot open output file: " +
                                     options.output.string());
        output << generated;
        if (!output)
            throw std::runtime_error("Error writing output file: " +
                                     options.output.string());
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "sbcsgen: " << error.what() << '\n';
        return 1;
    }
}
