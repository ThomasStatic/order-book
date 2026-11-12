#pragma once

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// A committed browser terminal session's transcript (tests/wasm/sessions/<name>.expected): each
// line typed after "> ", then its output, its stderr lines after "! ", and "[exit N]" when not 0.
// Read here for what its client commands printed, which send of the session's collected CSV must
// print too, and for its sends, which a native counterpart sends first. See
// docs/internals/wasm.md.
namespace order_book::wasm_testing {

    inline std::vector<std::string> linesOf(const std::string& text) {
        std::vector<std::string> lines;
        std::istringstream input(text);
        for (std::string line; std::getline(input, line);) {
            lines.push_back(line);
        }
        return lines;
    }

    inline std::vector<std::string> wordsOf(const std::string& line) {
        std::vector<std::string> words;
        std::istringstream input(line);
        for (std::string word; input >> word;) {
            words.push_back(word);
        }
        return words;
    }

    // One line typed and what it printed. words drops the program name a CLI command may keep.
    struct Typed {
        std::vector<std::string> words;
        std::string out;
        std::string err;
        int exit = 0;

        std::string verb() const { return words.empty() ? std::string() : words.front(); }
        bool isClientCommand() const {
            const std::string word = verb();
            return word == "buy" || word == "sell" || word == "cancel" || word == "replace";
        }
    };

    inline std::vector<Typed> commandsOf(const std::string& transcript) {
        std::vector<Typed> typed;
        for (const std::string& line : linesOf(transcript)) {
            if (line.starts_with("> ")) {
                Typed command;
                command.words = wordsOf(line.substr(2));
                if (command.words.size() > 1 && command.words.front() == "order_book_cli") {
                    command.words.erase(command.words.begin());
                }
                typed.push_back(std::move(command));
            } else if (typed.empty()) {
                continue;
            } else if (line.starts_with("! ")) {
                typed.back().err += line.substr(2) + "\n";
            } else if (line.starts_with("[exit ") && line.ends_with("]")) {
                typed.back().exit = std::stoi(line.substr(6, line.size() - 7));
            } else {
                typed.back().out += line + "\n";
            }
        }
        return typed;
    }

    using AnswersByLine = std::map<std::string, std::vector<std::string>>;

    // Each `line N:` line, by N, in the order printed.
    inline AnswersByLine answersByLine(const std::string& text) {
        AnswersByLine byLine;
        for (const std::string& line : linesOf(text)) {
            if (line.starts_with("line ")) {
                byLine[line.substr(0, line.find(':'))].push_back(line);
            }
        }
        return byLine;
    }

    // What the transcript's buy, sell, cancel, and replace lines printed, and whether any was
    // refused (exit 4), which is when send of the collected CSV exits 4.
    struct ClientAnswers {
        AnswersByLine byLine;
        bool refused = false;
    };

    inline ClientAnswers clientAnswersOf(const std::string& transcript) {
        ClientAnswers answers;
        std::string printed;
        for (const Typed& command : commandsOf(transcript)) {
            if (command.isClientCommand()) {
                printed += command.out;
                answers.refused = answers.refused || command.exit == 4;
            }
        }
        answers.byLine = answersByLine(printed);
        return answers;
    }

    // The session's sends, which a native counterpart sends before the collected CSV. That gives
    // the same books only if no client command came before a send, which this checks.
    struct Sends {
        std::vector<Typed> sends;
        bool allBeforeClientCommands = true;
    };

    inline Sends sendsOf(const std::string& transcript) {
        Sends found;
        bool clientSeen = false;
        for (const Typed& command : commandsOf(transcript)) {
            if (command.verb() == "send") {
                found.sends.push_back(command);
                found.allBeforeClientCommands = found.allBeforeClientCommands && !clientSeen;
            }
            clientSeen = clientSeen || command.isClientCommand();
        }
        return found;
    }

}  // namespace order_book::wasm_testing
