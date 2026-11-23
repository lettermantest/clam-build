// Agent: conversation state, tool execution, and the NVIDIA NIM turn loop.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "json.h"

namespace clam {

struct ToolResult {
    std::string output;
    std::vector<std::string> images;  // screenshot/image paths to attach
};

struct ActivityEntry {
    std::string label;
    std::string summary;
    std::string detail;
};

enum class Role { User, Assistant, Tool };

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;  // raw JSON text
};

struct Message {
    Role role = Role::User;
    std::string text;
    std::vector<std::string> images;
    std::string tool_call_id;
    std::string tool_name;
    std::vector<ToolCall> calls;
};

// Callbacks invoked from a worker thread. The UI layer marshals them back.
struct AgentCallbacks {
    std::function<void(const std::string& status)> on_status;
    std::function<void(const std::string& thinking)> on_thinking;
    std::function<void(const std::string& tool, const ToolResult& result)> on_tool;
    std::function<void(const std::string& answer)> on_answer;
    std::function<void(const std::string& error)> on_error;
    std::function<void()> on_done;
};

ToolResult execute_tool(const std::string& name,
                        const Json& arguments,
                        const std::string& project_root,
                        const std::string& api_key);

std::vector<std::string> fetch_models(const std::string& api_key, std::string& error);

std::string tools_json();
std::string system_prompt();

// Runs a complete turn: repeated model calls with tool execution until a final answer.
void run_turn(const std::string& api_key,
              const std::string& model,
              const std::string& effort,
              const std::string& project_root,
              std::vector<Message> history,
              const std::atomic<bool>& cancel,
              const AgentCallbacks& callbacks,
              std::vector<Message>* final_history);

}  // namespace clam
