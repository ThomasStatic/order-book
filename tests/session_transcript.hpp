#pragma once

#include <map>
#include <sstream>
#include <string>
#include <vector>

// A committed browser terminal session's transcript (tests/wasm/sessions/<name>.expected), read
// for what its client commands printed, which send of the session's collected CSV must print
// too: each line typed after "> ", then its output, its stderr lines after "! ", and "[exit N]"
// when not 0. See docs/internals/wasm.md.
namespace order_book::wasm_testing {

    inline std::vector<std::string> linesOf(const std::string& text) {
        std::vector<std::string> lines;
        std::istringstream input(text);
        for (std::string line; std::getline(input, line);) {
            lines.push_back(line);
        }
        return lines;
    }

    using AnswersByLine = std::map<std::string, std::vector<std::string>>;

    // Each `line N:` line, by N, in the order printed.
    inline AnswersByLine answersByLine(const std::vector<std::string>& lines) {
        AnswersByLine byLine;
        for (const std::string& line : lines) {
            if (line.starts_with("line ")) {
                byLine[line.substr(0, line.find(':'))].push_back(line);
            }
        }
        return byLine;
    }

    inline AnswersByLine answersByLine(const std::string& text) { return answersByLine(linesOf(text)); }

    // What the transcript's buy, sell, cancel, and replace lines printed, and whether any was
    // refused (exit 4), which is when send of the collected CSV exits 4.
    struct ClientAnswers {
        AnswersByLine byLine;
        bool refused = false;
    };

    inline ClientAnswers clientAnswersOf(const std::string& transcript) {
        ClientAnswers answers;
        std::vector<std::string> printed;
        bool client = false;
        for (const std::string& line : linesOf(transcript)) {
            if (line.starts_with("> ")) {
                const std::string typed = line.substr(2);
                const std::string verb = typed.substr(0, typed.find(' '));
                client = verb == "buy" || verb == "sell" || verb == "cancel" || verb == "replace";
            } else if (client) {
                printed.push_back(line);
                answers.refused = answers.refused || line == "[exit 4]";
            }
        }
        answers.byLine = answersByLine(printed);
        return answers;
    }

}  // namespace order_book::wasm_testing
