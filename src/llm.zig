const std = @import("std");

pub const Config = struct {
    api_key: []const u8,
    base_url: []const u8,
    model: []const u8,
};

pub const ToolCall = struct {
    id: []const u8,
    name: []const u8,
    /// JSON object as a string, e.g. `{"command":"gcc --version"}`.
    arguments: []const u8,
};

pub const Message = struct {
    role: []const u8, // "system" | "user" | "assistant" | "tool"
    content: ?[]const u8 = null,
    tool_call_id: ?[]const u8 = null,
    tool_calls: []const ToolCall = &.{},
};

pub const Response = struct {
    content: ?[]const u8,
    tool_calls: []const ToolCall,
};

/// Sends one chat-completions request and parses the response.
///
/// `gpa` is used only for the short-lived request body; everything that must
/// outlive this call (URL, auth header, response body, parsed strings) is
/// allocated from `arena`, which is expected to live for the whole process.
pub fn chat(
    gpa: std.mem.Allocator,
    io: std.Io,
    arena: std.mem.Allocator,
    cfg: Config,
    messages: []const Message,
    tools_json: []const u8,
) !Response {
    const body = try buildRequest(gpa, cfg, messages, tools_json);
    defer gpa.free(body);

    const url = try std.fmt.allocPrint(arena, "{s}/chat/completions", .{cfg.base_url});
    const auth = try std.fmt.allocPrint(arena, "Bearer {s}", .{cfg.api_key});

    var client: std.http.Client = .{ .allocator = gpa, .io = io };
    defer client.deinit();

    // Arena-backed so the response body stays valid for the process lifetime.
    var resp_buf: std.Io.Writer.Allocating = .init(arena);

    const result = client.fetch(.{
        .location = .{ .url = url },
        .method = .POST,
        .payload = body,
        .extra_headers = &.{
            .{ .name = "content-type", .value = "application/json" },
            .{ .name = "authorization", .value = auth },
        },
        .response_writer = &resp_buf.writer,
    }) catch |err| {
        std.debug.print("http request failed: {s}\n", .{@errorName(err)});
        return err;
    };

    const status: u16 = @intFromEnum(result.status);
    if (status != 200) {
        std.debug.print("http status {d}:\n{s}\n", .{ status, resp_buf.written() });
        return error.HttpStatus;
    }

    return parseResponse(arena, resp_buf.written());
}

fn buildRequest(
    gpa: std.mem.Allocator,
    cfg: Config,
    messages: []const Message,
    tools_json: []const u8,
) ![]u8 {
    var buf: std.Io.Writer.Allocating = .init(gpa);
    errdefer buf.deinit();
    const w = &buf.writer;

    try w.writeAll("{\"model\":");
    try writeJsonString(w, cfg.model);
    try w.writeAll(",\"messages\":[");

    for (messages, 0..) |m, i| {
        if (i != 0) try w.writeAll(",");
        try w.writeAll("{\"role\":");
        try writeJsonString(w, m.role);

        if (m.content) |c| {
            try w.writeAll(",\"content\":");
            try writeJsonString(w, c);
        }
        if (m.tool_call_id) |id| {
            try w.writeAll(",\"tool_call_id\":");
            try writeJsonString(w, id);
        }
        if (m.tool_calls.len != 0) {
            try w.writeAll(",\"tool_calls\":[");
            for (m.tool_calls, 0..) |tc, j| {
                if (j != 0) try w.writeAll(",");
                try w.writeAll("{\"id\":");
                try writeJsonString(w, tc.id);
                try w.writeAll(",\"type\":\"function\",\"function\":{\"name\":");
                try writeJsonString(w, tc.name);
                try w.writeAll(",\"arguments\":");
                try writeJsonString(w, tc.arguments);
                try w.writeAll("}}");
            }
            try w.writeAll("]");
        }

        try w.writeAll("}");
    }

    try w.writeAll("],\"tools\":");
    try w.writeAll(tools_json);
    try w.writeAll("}");

    return buf.toOwnedSlice();
}

fn parseResponse(arena: std.mem.Allocator, body: []const u8) !Response {
    const root = try std.json.parseFromSliceLeaky(std.json.Value, arena, body, .{});

    const choices = getField(root, "choices") orelse return error.InvalidResponse;
    const items = switch (choices) {
        .array => |a| a.items,
        else => return error.InvalidResponse,
    };
    if (items.len == 0) return error.InvalidResponse;

    const msg = getField(items[0], "message") orelse return error.InvalidResponse;
    const content: ?[]const u8 = getString(msg, "content");

    var tool_calls: std.ArrayList(ToolCall) = .empty;
    if (getField(msg, "tool_calls")) |tc| {
        const calls = switch (tc) {
            .array => |a| a.items,
            else => &.{},
        };
        for (calls) |item| {
            const id = getString(item, "id") orelse return error.InvalidResponse;
            const func = getField(item, "function") orelse return error.InvalidResponse;
            const name = getString(func, "name") orelse return error.InvalidResponse;
            const arguments = getString(func, "arguments") orelse "";
            try tool_calls.append(arena, .{ .id = id, .name = name, .arguments = arguments });
        }
    }

    return .{ .content = content, .tool_calls = tool_calls.items };
}

fn getField(v: std.json.Value, key: []const u8) ?std.json.Value {
    switch (v) {
        .object => |obj| return obj.get(key),
        else => return null,
    }
}

fn getString(v: std.json.Value, key: []const u8) ?[]const u8 {
    const f = getField(v, key) orelse return null;
    return switch (f) {
        .string => |s| s,
        else => null,
    };
}

fn writeJsonString(w: *std.Io.Writer, s: []const u8) !void {
    const hex = "0123456789abcdef";
    try w.writeByte('"');
    for (s) |c| {
        if (c == '"') {
            try w.writeAll("\\\"");
        } else if (c == '\\') {
            try w.writeAll("\\\\");
        } else if (c == '\n') {
            try w.writeAll("\\n");
        } else if (c == '\r') {
            try w.writeAll("\\r");
        } else if (c == '\t') {
            try w.writeAll("\\t");
        } else if (c == 0x08) {
            try w.writeAll("\\b");
        } else if (c == 0x0c) {
            try w.writeAll("\\f");
        } else if (c < 0x20) {
            try w.writeAll("\\u00");
            try w.writeByte(hex[c >> 4]);
            try w.writeByte(hex[c & 0xf]);
        } else {
            try w.writeByte(c);
        }
    }
    try w.writeByte('"');
}
