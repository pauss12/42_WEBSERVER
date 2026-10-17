# Agent Customizations Created

The monolithic `webserv-rules.md` file has been successfully split and migrated into the new Agent Customization System structure. Your project is now well-prepared for adding more plugins, hooks, or a full `mcp_config.json` in the future!

## What Changed?

1. **`persona.md`**: Extracted the "Systems Programming Teacher" instructions into [`persona.md`](file:///Users/p/Desktop/webserv-42/.agents/rules/persona.md). This rule will always be loaded automatically in this project directory to ensure I consistently act as your teacher.
2. **`project-constraints.md`**: Extracted the 42 project requirements (C++98 enforcement, specific function whitelists, config requirements) into [`project-constraints.md`](file:///Users/p/Desktop/webserv-42/.agents/rules/project-constraints.md). This rule acts as a global constraint on how I write and review code for you.
3. **`non-blocking-io` Skill**: Created a dedicated skill for the complex multiplexer I/O logic in [`SKILL.md`](file:///Users/p/Desktop/webserv-42/.agents/skills/non-blocking-io/SKILL.md). This means whenever you ask me to help with `kqueue`, `epoll`, or reading/writing sockets, I can instantly pull these strict non-blocking rules into my context.
4. **Cleanup**: Removed the old `.agents/rules/webserv-rules.md` to prevent duplicate instructions.

> [!TIP]
> Since `.agents` rules apply dynamically, these rules will seamlessly load anytime we discuss this codebase. If you create a new skill later on (like `cgi-execution`), you can just create a new folder under `.agents/skills/` without bloating my default memory context!
