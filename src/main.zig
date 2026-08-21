const std = @import("std");
const llm = @import("llm.zig");
const tools = @import("tools.zig");
const build_options = @import("build_options");

const SYSTEM_PROMPT =
    "You are a coding agent. Solve the user's task by reading and writing files and running commands. " ++
    "Use the provided tools to inspect and change the codebase, then run the build/tests to verify your work. " ++
    "When you are done, answer concisely with what you changed.";

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;
    const io = init.io;
    const arena = init.arena.allocator();

    const cfg = llm.Config{
        .api_key = init.environ_map.get("LLM_API_KEY") orelse build_options.llm_api_key orelse {
            std.debug.print("error: LLM_API_KEY is not set\n", .{});
            return error.MissingApiKey;
        },
        .base_url = init.environ_map.get("LLM_BASE_URL") orelse build_options.llm_base_url orelse "https://api.openai.com/v1",
        .model = init.environ_map.get("LLM_MODEL") orelse build_options.llm_model orelse "gpt-4o-mini",
    };

    const max_steps = std.fmt.parseInt(u32, init.environ_map.get("LLM_MAX_STEPS") orelse "8", 10) catch 8;

    const args = try init.minimal.args.toSlice(arena);
    const task: []const u8 = if (args.len > 1) args[1] else try readStdin(arena, io);

    var messages: std.ArrayList(llm.Message) = .empty;
    try messages.append(arena, .{ .role = "system", .content = SYSTEM_PROMPT });
    try messages.append(arena, .{ .role = "user", .content = task });

    var steps: u32 = 0;
    while (steps < max_steps) : (steps += 1) {
        const resp = try llm.chat(gpa, io, arena, cfg, messages.items, tools.DEFINITIONS_JSON);

        if (resp.tool_calls.len == 0) {
            const final = try std.fmt.allocPrint(arena, "{s}\n", .{resp.content orelse ""});
            try writeStdout(io, final);
            return;
        }

        try messages.append(arena, .{
            .role = "assistant",
            .content = resp.content,
            .tool_calls = resp.tool_calls,
        });

        for (resp.tool_calls) |tc| {
            const result = try tools.run(gpa, io, arena, tc.name, tc.arguments);
            try messages.append(arena, .{
                .role = "tool",
                .tool_call_id = tc.id,
                .content = result,
            });
        }
    }

    std.debug.print("stopped after {d} steps without a final answer\n", .{max_steps});
    return error.MaxStepsReached;
}

fn readStdin(arena: std.mem.Allocator, io: std.Io) ![]const u8 {
    var buf: [4096]u8 = undefined;
    var reader = std.Io.File.stdin().reader(io, &buf);
    return reader.interface.allocRemaining(arena, std.Io.Limit.limited(1024 * 1024));
}

fn writeStdout(io: std.Io, bytes: []const u8) !void {
    var buf: [4096]u8 = undefined;
    var file_writer: std.Io.File.Writer = .init(std.Io.File.stdout(), io, &buf);
    const w = &file_writer.interface;
    try w.writeAll(bytes);
    try w.flush();
}
