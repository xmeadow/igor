const std = @import("std");
const builtin = @import("builtin");

pub const RUN_COMMAND = "run_command";
pub const READ_FILE = "read_file";
pub const WRITE_FILE = "write_file";

/// Tool definitions sent verbatim to the LLM as a JSON array.
pub const DEFINITIONS_JSON =
    \\[
    \\  {"type":"function","function":{"name":"run_command","description":"Run a shell command and return its exit status, stdout and stderr. Use this to compile, run tests, or inspect the environment.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"The command line to run."}},"required":["command"]}}},
    \\  {"type":"function","function":{"name":"read_file","description":"Read a UTF-8 text file and return its contents.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"Path to the file, relative to the current directory."}},"required":["path"]}}},
    \\  {"type":"function","function":{"name":"write_file","description":"Create or overwrite a file with the given contents.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"Path to the file, relative to the current directory."},"content":{"type":"string","description":"Full contents of the file."}},"required":["path","content"]}}}
    \\]
;

/// Executes a single tool call and returns the result string to feed back to
/// the model. The result is allocated from `arena`.
pub fn run(
    gpa: std.mem.Allocator,
    io: std.Io,
    arena: std.mem.Allocator,
    name: []const u8,
    arguments_json: []const u8,
) ![]const u8 {
    const args_text = if (arguments_json.len == 0) "{}" else arguments_json;
    const args = try std.json.parseFromSliceLeaky(std.json.Value, arena, args_text, .{});

    if (std.mem.eql(u8, name, RUN_COMMAND)) return runCommand(gpa, io, arena, args);
    if (std.mem.eql(u8, name, READ_FILE)) return readFile(gpa, io, arena, args);
    if (std.mem.eql(u8, name, WRITE_FILE)) return writeFile(io, arena, args);
    return std.fmt.allocPrint(arena, "unknown tool: {s}", .{name});
}

fn runCommand(
    gpa: std.mem.Allocator,
    io: std.Io,
    arena: std.mem.Allocator,
    args: std.json.Value,
) ![]const u8 {
    const command = getString(args, "command") orelse return error.MissingArgument;

    const argv_windows = [5][]const u8{ "cmd.exe", "/d", "/s", "/c", command };
    const argv_posix = [3][]const u8{ "/bin/sh", "-c", command };
    const argv: []const []const u8 = if (builtin.os.tag == .windows) &argv_windows else &argv_posix;

    const result = std.process.run(gpa, io, .{
        .argv = argv,
        .stdout_limit = std.Io.Limit.limited(1024 * 1024),
        .stderr_limit = std.Io.Limit.limited(1024 * 1024),
    }) catch |err| {
        return std.fmt.allocPrint(arena, "failed to run command: {s}", .{@errorName(err)});
    };
    defer gpa.free(result.stdout);
    defer gpa.free(result.stderr);

    const status = switch (result.term) {
        .exited => |code| try std.fmt.allocPrint(arena, "exit code {d}", .{code}),
        .signal => |sig| try std.fmt.allocPrint(arena, "terminated by signal {d}", .{@intFromEnum(sig)}),
        .stopped => |sig| try std.fmt.allocPrint(arena, "stopped by signal {d}", .{@intFromEnum(sig)}),
        .unknown => |code| try std.fmt.allocPrint(arena, "unknown status {d}", .{code}),
    };

    var out: std.Io.Writer.Allocating = .init(arena);
    const w = &out.writer;
    try w.print("STATUS: {s}\n--- STDOUT ---\n", .{status});
    try w.writeAll(result.stdout);
    try w.writeAll("\n--- STDERR ---\n");
    try w.writeAll(result.stderr);
    return out.toOwnedSlice();
}

fn readFile(
    gpa: std.mem.Allocator,
    io: std.Io,
    arena: std.mem.Allocator,
    args: std.json.Value,
) ![]const u8 {
    const path = getString(args, "path") orelse return error.MissingArgument;

    const dir = std.Io.Dir.cwd();
    const contents = std.Io.Dir.readFileAlloc(dir, io, path, gpa, std.Io.Limit.limited(1024 * 1024)) catch |err| {
        return std.fmt.allocPrint(arena, "read_file failed: {s}", .{@errorName(err)});
    };
    defer gpa.free(contents);

    return arena.dupe(u8, contents);
}

fn writeFile(io: std.Io, arena: std.mem.Allocator, args: std.json.Value) ![]const u8 {
    const path = getString(args, "path") orelse return error.MissingArgument;
    const content = getString(args, "content") orelse return error.MissingArgument;

    const dir = std.Io.Dir.cwd();
    std.Io.Dir.writeFile(dir, io, .{ .sub_path = path, .data = content }) catch |err| {
        return std.fmt.allocPrint(arena, "write_file failed: {s}", .{@errorName(err)});
    };

    return arena.dupe(u8, "ok");
}

fn getString(v: std.json.Value, key: []const u8) ?[]const u8 {
    switch (v) {
        .object => |obj| {
            const f = obj.get(key) orelse return null;
            return switch (f) {
                .string => |s| s,
                else => null,
            };
        },
        else => return null,
    }
}
