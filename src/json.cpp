#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace ap {

namespace {

std::string jsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string timeStamp() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

void appendHexByte(std::string& out, uint8_t value) {
    static const char* digits = "0123456789abcdef";
    out.push_back(digits[value >> 4]);
    out.push_back(digits[value & 0x0f]);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

class JsonReader {
public:
    explicit JsonReader(const fs::path& path) : path_(path) {
        in_.open(path, std::ios::binary);
        check(in_.is_open(), "Could not open JSON scene: " + pathToUtf8(path));
    }

    Scene parse() {
        Scene scene;
        skipWhitespace();
        expect('{');
        skipWhitespace();
        if (peek() == '}') {
            take();
        } else {
            for (;;) {
                std::string key = parseString();
                skipWhitespace();
                expect(':');
                skipWhitespace();
                if (key == "format" || key == "version" || key == "created" ||
                    key == "audio" || key == "notes") {
                    skipValue();
                } else if (key == "width") {
                    scene.width = static_cast<int>(parseNumber());
                } else if (key == "height") {
                    scene.height = static_cast<int>(parseNumber());
                } else if (key == "fps") {
                    scene.fps = parseNumber();
                } else if (key == "duration") {
                    scene.duration = parseNumber();
                } else if (key == "source") {
                    scene.source = parseString();
                } else if (key == "charset") {
                    scene.charset = parseString();
                } else if (key == "colorMode") {
                    scene.colorMode = parseString();
                } else if (key == "frames") {
                    parseFrames(scene);
                } else {
                    skipValue();
                }
                skipWhitespace();
                int c = peek();
                if (c == ',') {
                    take();
                    skipWhitespace();
                    continue;
                }
                expect('}');
                break;
            }
        }
        skipWhitespace();
        if (peek() != -1) fail("Unexpected data after the top-level object");

        check(scene.width > 0 && scene.width <= 500, "JSON scene has an invalid width.");
        check(scene.height > 0 && scene.height <= 300, "JSON scene has an invalid height.");
        check(!scene.frames.empty(), "JSON scene contains no frames.");
        check(scene.fps > 0.0 && scene.fps <= 240.0, "JSON scene has an invalid fps.");
        if (scene.charset.empty()) scene.charset = kDefaultCharset;
        bool anyColors = !iequals(scene.colorMode, "mono");
        double lastTime = 0.0;
        for (size_t i = 0; i < scene.frames.size(); ++i) {
            SceneFrame& frame = scene.frames[i];
            check(static_cast<int>(frame.rows.size()) == scene.height,
                  "JSON frame " + std::to_string(i) + " has the wrong row count.");
            for (const auto& row : frame.rows) {
                check(static_cast<int>(row.size()) == scene.width,
                      "JSON frame " + std::to_string(i) +
                          " has a row with the wrong width.");
            }
            if (anyColors) {
                check(static_cast<int>(frame.colors.size()) == scene.width * scene.height,
                      "JSON frame " + std::to_string(i) +
                          " has the wrong color count.");
            }
            if (frame.time < 0.0) frame.time = static_cast<double>(i) / scene.fps;
            lastTime = std::max(lastTime, frame.time);
            (void)anyColors;
        }
        scene.hasColors = anyColors;
        if (scene.duration <= 0.0) scene.duration = lastTime + 1.0 / scene.fps;
        return scene;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("JSON scene error at byte " + std::to_string(offset_) +
                                 ": " + message);
    }

    int peek() {
        return in_.peek();
    }

    int take() {
        int c = in_.get();
        if (c != EOF) ++offset_;
        return c;
    }

    void skipWhitespace() {
        for (;;) {
            int c = peek();
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                take();
            } else {
                break;
            }
        }
    }

    void expect(char wanted) {
        int c = take();
        if (c != static_cast<unsigned char>(wanted))
            fail(std::string("expected '") + wanted + "'");
    }

    static void appendUtf8(std::string& out, unsigned codePoint) {
        if (codePoint <= 0x7F) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    unsigned readHex4() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            int c = take();
            int digit = hexValue(static_cast<char>(c));
            if (digit < 0) fail("invalid \\u escape");
            value = (value << 4) | static_cast<unsigned>(digit);
        }
        return value;
    }

    std::string parseString() {
        skipWhitespace();
        expect('"');
        std::string out;
        for (;;) {
            int c = take();
            if (c == EOF) fail("unterminated string");
            if (c == '"') break;
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            int escape = take();
            switch (escape) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    unsigned cp = readHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && peek() == '\\') {
                        take();
                        if (take() == 'u') {
                            unsigned low = readHex4();
                            if (low >= 0xDC00 && low <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    fail("invalid escape in string");
            }
        }
        return out;
    }

    double parseNumber() {
        skipWhitespace();
        std::string text;
        while (true) {
            int c = peek();
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' ||
                c == 'E') {
                text.push_back(static_cast<char>(take()));
            } else {
                break;
            }
        }
        if (text.empty()) fail("expected a number");
        try {
            return std::stod(text);
        } catch (...) {
            fail("invalid number");
        }
    }

    void skipValue() {
        skipWhitespace();
        int c = peek();
        if (c == '"') {
            parseString();
        } else if (c == '{') {
            take();
            skipWhitespace();
            if (peek() == '}') {
                take();
                return;
            }
            for (;;) {
                parseString();
                skipWhitespace();
                expect(':');
                skipValue();
                skipWhitespace();
                int next = peek();
                if (next == ',') {
                    take();
                    skipWhitespace();
                    continue;
                }
                expect('}');
                break;
            }
        } else if (c == '[') {
            take();
            skipWhitespace();
            if (peek() == ']') {
                take();
                return;
            }
            for (;;) {
                skipValue();
                skipWhitespace();
                int next = peek();
                if (next == ',') {
                    take();
                    skipWhitespace();
                    continue;
                }
                expect(']');
                break;
            }
        } else if (c == 't') {
            for (char expected : std::string("true"))
                if (take() != expected) fail("invalid literal");
        } else if (c == 'f') {
            for (char expected : std::string("false"))
                if (take() != expected) fail("invalid literal");
        } else if (c == 'n') {
            for (char expected : std::string("null"))
                if (take() != expected) fail("invalid literal");
        } else {
            parseNumber();
        }
    }

    std::vector<std::string> parseStringArray() {
        std::vector<std::string> result;
        skipWhitespace();
        expect('[');
        skipWhitespace();
        if (peek() == ']') {
            take();
            return result;
        }
        for (;;) {
            result.push_back(parseString());
            skipWhitespace();
            int c = peek();
            if (c == ',') {
                take();
                skipWhitespace();
                continue;
            }
            expect(']');
            break;
        }
        return result;
    }

    void parseFrames(Scene& scene) {
        skipWhitespace();
        expect('[');
        skipWhitespace();
        if (peek() == ']') {
            take();
            return;
        }
        for (;;) {
            SceneFrame frame;
            frame.time = -1.0;
            skipWhitespace();
            expect('{');
            skipWhitespace();
            if (peek() == '}') {
                take();
            } else {
                for (;;) {
                    std::string key = parseString();
                    skipWhitespace();
                    expect(':');
                    skipWhitespace();
                    if (key == "t") {
                        frame.time = parseNumber();
                    } else if (key == "c") {
                        frame.rows = parseStringArray();
                    } else if (key == "p") {
                        std::vector<std::string> colorRows = parseStringArray();
                        bool anyNonZero = false;
                        for (const auto& row : colorRows) {
                            check(row.size() % 2 == 0, "JSON color row has odd length.");
                            for (size_t i = 0; i + 1 < row.size(); i += 2) {
                                int hi = hexValue(row[i]);
                                int lo = hexValue(row[i + 1]);
                                check(hi >= 0 && lo >= 0, "JSON color row is not hex.");
                                uint8_t value = static_cast<uint8_t>((hi << 4) | lo);
                                frame.colors.push_back(value);
                                anyNonZero = anyNonZero || value != 0;
                            }
                        }
                        if (colorRows.empty()) frame.colors.clear();
                        (void)anyNonZero;
                    } else {
                        skipValue();
                    }
                    skipWhitespace();
                    int c = peek();
                    if (c == ',') {
                        take();
                        skipWhitespace();
                        continue;
                    }
                    expect('}');
                    break;
                }
            }
            scene.frames.push_back(std::move(frame));
            skipWhitespace();
            int c = peek();
            if (c == ',') {
                take();
                skipWhitespace();
                continue;
            }
            expect(']');
            break;
        }
    }

    fs::path path_;
    std::ifstream in_;
    uint64_t offset_ = 0;
};

}  // namespace

SceneWriter::SceneWriter(const fs::path& path, int width, int height, double fps,
                         double duration, const std::string& source,
                         const std::string& charset, const std::string& colorMode)
    : path_(path),
      width_(width),
      height_(height),
      source_(source),
      charset_(charset),
      colorMode_(colorMode) {
    out_.open(path, std::ios::binary | std::ios::trunc);
    check(out_.is_open(), "Could not create JSON scene: " + pathToUtf8(path));
    std::ostringstream header;
    header << "{\n";
    header << "  \"format\": \"ascii-player-scene\",\n";
    header << "  \"version\": 1,\n";
    header << "  \"created\": \"" << timeStamp() << "\",\n";
    header << "  \"source\": \"" << jsonEscape(source) << "\",\n";
    header << "  \"width\": " << width << ",\n";
    header << "  \"height\": " << height << ",\n";
    header << "  \"fps\": " << std::fixed << std::setprecision(3) << fps << ",\n";
    header << "  \"duration\": " << std::fixed << std::setprecision(3) << duration
           << ",\n";
    header << "  \"charset\": \"" << jsonEscape(charset) << "\",\n";
    header << "  \"colorMode\": \"" << jsonEscape(colorMode) << "\",\n";
    header << "  \"frames\": [\n";
    write(header.str());
}

SceneWriter::~SceneWriter() {
    try {
        finish();
    } catch (...) {
    }
}

void SceneWriter::write(const std::string& text) {
    out_.write(text.data(), static_cast<std::streamsize>(text.size()));
    check(out_.good(), "Failed while writing JSON scene.");
    bytesWritten_ += text.size();
}

void SceneWriter::writeFrame(const AsciiFrame& frame, double timeSeconds) {
    std::string text;
    text.reserve(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4 + 256);
    if (frameCount_ > 0) text += ",\n";
    text += "  {\"t\":";
    char timeBuffer[64];
    std::snprintf(timeBuffer, sizeof(timeBuffer), "%.4f", timeSeconds);
    text += timeBuffer;
    text += ",\"c\":[";
    for (int y = 0; y < height_; ++y) {
        if (y) text.push_back(',');
        text.push_back('"');
        const std::string& row = frame.rows[static_cast<size_t>(y)];
        for (int x = 0; x < width_; ++x) {
            char c = x < static_cast<int>(row.size()) ? row[static_cast<size_t>(x)] : ' ';
            if (c == '"' || c == '\\') text.push_back('\\');
            if (c < 0x20) c = ' ';
            text.push_back(c);
        }
        text.push_back('"');
    }
    text += "]";
    if (!iequals(colorMode_, "mono")) {
        text += ",\"p\":[";
        for (int y = 0; y < height_; ++y) {
            if (y) text.push_back(',');
            text.push_back('"');
            for (int x = 0; x < width_; ++x) {
                int index = quantizeXterm(frame.colorAt(x, y));
                appendHexByte(text, static_cast<uint8_t>(index));
            }
            text.push_back('"');
        }
        text += "]";
    }
    text += "}";
    write(text);
    ++frameCount_;
}

void SceneWriter::finish() {
    if (finished_) return;
    finished_ = true;
    write("\n  ]\n}\n");
    out_.flush();
    check(out_.good(), "Failed to finalize JSON scene.");
    out_.close();
}

Scene loadScene(const fs::path& path) {
    std::error_code ec;
    check(fs::is_regular_file(path, ec), "JSON scene not found: " + pathToUtf8(path));
    JsonReader reader(path);
    return reader.parse();
}

AsciiFrame sceneFrameToAscii(const Scene& scene, const SceneFrame& source) {
    AsciiFrame frame;
    frame.reset(scene.width, scene.height);
    for (int y = 0; y < scene.height; ++y) {
        const std::string& row = source.rows[static_cast<size_t>(y)];
        for (int x = 0; x < scene.width; ++x) {
            char c = x < static_cast<int>(row.size()) ? row[static_cast<size_t>(x)] : ' ';
            RGB color{255, 255, 255};
            if (scene.hasColors &&
                static_cast<int>(source.colors.size()) == scene.width * scene.height) {
                color = xtermColor(source.colors[static_cast<size_t>(y) * scene.width + x]);
            }
            frame.set(x, y, c, color);
        }
    }
    return frame;
}

}  // namespace ap
