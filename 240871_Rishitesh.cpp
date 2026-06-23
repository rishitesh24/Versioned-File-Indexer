#include <iostream>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <cctype>
#include <cassert>

using namespace std;

// Demonstrates domain-specific exception handling
class AnalyzerException : public runtime_error {
public:
    AnalyzerException(const string& msg) : runtime_error(msg) {}
};

class BufferLimitException : public AnalyzerException {
public:
    BufferLimitException()
        : AnalyzerException("Buffer size must be between 256 and 1024 KB.") {}
};

// CONFIG STRUCT 
struct Config {
    string file, file1, file2;
    string version, version1, version2;
    string queryType, word;
    int topK = 0;
    size_t bufferSize = 0;
};


// Ensures case-insensitive word matching 
string toLowerString(const string& s) {
    string result = s;
    for (char& c : result)
        c = tolower(static_cast<unsigned char>(c));
    return result;
}


// Satisfies the user-defined template requirement
template <typename Container>
class QueryResultPrinter {
public:
    static void printTopK(const Container& container, int k) {
        int count = 0;
        for (const auto& item : container) {
            if (count >= k) break;
            cout << item.first << " " << item.second << endl;
            count++;
        }
    }
};


// Handles fixed-buffer file I/O strictly 
class BufferedFileReader {
private:
    ifstream file;
    size_t bufferSize;
    char* buffer;
    streamsize bytesRead;

public:
    BufferedFileReader(const string& path, size_t bufferSize) {
        this->bufferSize = bufferSize;
        bytesRead = 0;

        file.open(path, ios::in | ios::binary);
        if (!file)
            throw AnalyzerException("Failed to open file: " + path);

        buffer = new char[bufferSize];
    }

    // Defensive Programming: Prevent double-free errors from shallow copies
    BufferedFileReader(const BufferedFileReader&) = delete;
    BufferedFileReader& operator=(const BufferedFileReader&) = delete;

    ~BufferedFileReader() {
        delete[] buffer;
        if (file.is_open())
            file.close();
    }

    bool readChunk() {
        if (!file.good())
            return false;

        file.read(buffer, bufferSize);
        bytesRead = file.gcount();
        return bytesRead > 0;
    }

    const char* getBuffer() const { return buffer; }
    streamsize getBytesRead() const { return bytesRead; }
};


// Encapsulates the unordered_map to prevent Primitive Obsession
class VersionIndex {
private:
    string versionName;
    unordered_map<string, int> wordCounts;

public:
    VersionIndex() = default;

    VersionIndex(const string& name) {
        versionName = name;
        wordCounts.reserve(500000); 
    }

    void addWord(const string& word) {
        wordCounts[word]++;
    }

    void addWord(const vector<string>& words) {
        for (const auto& w : words) addWord(w);
    }

    int getCount(const string& word) const {
        auto it = wordCounts.find(word);
        if (it != wordCounts.end())
            return it->second;
        return 0;
    }

    const unordered_map<string, int>& getData() const {
        return wordCounts;
    }

    friend ostream& operator<<(ostream& os, const VersionIndex& vi);
};

ostream& operator<<(ostream& os, const VersionIndex& vi) {
    os << "Version: " << vi.versionName;
    return os;
}

// VERSIONED INDEXER 
class VersionedIndexer {
private:
    unordered_map<string, VersionIndex> indices;

public:
    void createVersion(const string& version) {
        if (indices.find(version) == indices.end()) {
            indices[version] = VersionIndex(version);
        }
    }

    VersionIndex& getVersionMutable(const string& version) {
        auto it = indices.find(version);
        if (it == indices.end())
            throw AnalyzerException("Version not found: " + version);
        return it->second;
    }

    const VersionIndex& getVersion(const string& version) const {
        auto it = indices.find(version);
        if (it == indices.end())
            throw AnalyzerException("Version not found: " + version);
        return it->second;
    }
};

// TOKENIZER 
class Tokenizer {
private:
    string leftover;
    string current;

public:
    Tokenizer() {
        leftover.reserve(256);
        current.reserve(256); // Optimization: Prevents string reallocations
    }

    void processChunk(const char* buffer, streamsize size, VersionIndex& index) {
        current = leftover;
        leftover.clear();

        for (streamsize i = 0; i < size; ++i) {
            unsigned char c = buffer[i];

            if (isalnum(c)) {
                if (c >= 'A' && c <= 'Z')
                    c += 32;
                current.push_back(c);
            }
            else {
                if (!current.empty()) {
                    index.addWord(current);
                    current.clear();
                }
            }
        }

        if (!current.empty())
            leftover = current;
    }

    void flush(VersionIndex& index) {
        if (!leftover.empty()) {
            index.addWord(leftover);
            leftover.clear();
        }
    }
};

// QUERY BASE 
class Query {
public:
    virtual void execute() const = 0;
    virtual ~Query() = default;
};

// WORD QUERY 
class WordQuery : public Query {
private:
    const VersionedIndexer& indexer;
    string version, word;

public:
    WordQuery(const VersionedIndexer& idx, const string& v, const string& w)
        : indexer(idx), version(v), word(w) {}

    void execute() const override {
        const VersionIndex& vi = indexer.getVersion(version);
        cout << vi << endl;
        cout << "Count: " << vi.getCount(word) << endl;
    }
};

// DIFF QUERY 
class DiffQuery : public Query {
private:
    const VersionedIndexer& indexer;
    string version1, version2, word;

public:
    DiffQuery(const VersionedIndexer& idx,
              const string& v1,
              const string& v2,
              const string& w)
        : indexer(idx), version1(v1), version2(v2), word(w) {}

    void execute() const override {
        int c1 = indexer.getVersion(version1).getCount(word);
        int c2 = indexer.getVersion(version2).getCount(word);
        cout << "Difference (" << version2 << " - " << version1 << "): "
             << (c2 - c1) << endl;
    }
};

// TOP-K QUERY 
class TopKQuery : public Query {
private:
    const VersionedIndexer& indexer;
    string version;
    int k;

public:
    TopKQuery(const VersionedIndexer& idx, const string& v, int top)
        : indexer(idx), version(v), k(top) {}

    void execute() const override {
        const auto& data = indexer.getVersion(version).getData();
        vector<pair<string, int>> vec(data.begin(), data.end());

        sort(vec.begin(), vec.end(),
             [](const pair<string, int>& a, const pair<string, int>& b) {
                 return a.second > b.second;
             });

        cout << "Top-" << k << " words in version " << version << ":" << endl;
        QueryResultPrinter<vector<pair<string,int>>>::printTopK(vec, k);
    }
};

// QUERY FACTORY 
Query* createQuery(const Config& config, const VersionedIndexer& indexer) {
    if (config.queryType == "word")
        return new WordQuery(indexer, config.version, config.word);

    if (config.queryType == "diff")
        return new DiffQuery(indexer, config.version1, config.version2, config.word);

    if (config.queryType == "top")
        return new TopKQuery(indexer, config.version, config.topK);

    throw AnalyzerException("Invalid query type.");
}

// ARGUMENT PARSER 
Config parseArguments(int argc, char* argv[]) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];

        if (i + 1 >= argc)
            throw AnalyzerException("Missing value for argument: " + arg);

        if (arg == "--file") config.file = argv[++i];
        else if (arg == "--file1") config.file1 = argv[++i];
        else if (arg == "--file2") config.file2 = argv[++i];
        else if (arg == "--version") config.version = argv[++i];
        else if (arg == "--version1") config.version1 = argv[++i];
        else if (arg == "--version2") config.version2 = argv[++i];
        else if (arg == "--buffer") config.bufferSize = stoi(argv[++i]) * 1024;
        else if (arg == "--query") config.queryType = argv[++i];
        else if (arg == "--word") config.word = toLowerString(argv[++i]);
        else if (arg == "--top") config.topK = stoi(argv[++i]);
    }

    if (config.bufferSize < 256 * 1024 || config.bufferSize > 1024 * 1024)
        throw BufferLimitException();

    return config;
}

// MAIN 
int main(int argc, char* argv[]) {
    try {
        auto start = chrono::high_resolution_clock::now();

        Config config = parseArguments(argc, argv);

        // Defensive Programming check
        assert(config.bufferSize >= 256 * 1024 && config.bufferSize <= 1024 * 1024);

        VersionedIndexer indexer;

        auto processFile = [&](const string& path, const string& version) {
            indexer.createVersion(version);
            BufferedFileReader reader(path, config.bufferSize);
            Tokenizer tokenizer;
            VersionIndex& vIndex = indexer.getVersionMutable(version);

            while (reader.readChunk()) {
                tokenizer.processChunk(
                    reader.getBuffer(),
                    reader.getBytesRead(),
                    vIndex
                );
            }
            tokenizer.flush(vIndex);
        };

        if (config.queryType == "word" || config.queryType == "top") {
            processFile(config.file, config.version);
        }
        else if (config.queryType == "diff") {
            processFile(config.file1, config.version1);
            processFile(config.file2, config.version2);
        }

        Query* query = createQuery(config, indexer);
        query->execute();
        delete query;

        auto end = chrono::high_resolution_clock::now();
        double duration = chrono::duration<double>(end - start).count();

        cout << "Buffer Size (KB): " << config.bufferSize / 1024 << endl;
        cout << "Execution Time (s): " << duration << endl;
    }
    catch (const exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}