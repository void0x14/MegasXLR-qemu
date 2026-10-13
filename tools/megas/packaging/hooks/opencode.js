// SPDX-License-Identifier: GPL-2.0-or-later
// Uses OpenCode's existing Bun runtime. No npm package or subprocess shell.
// Put this in the VM work project's .opencode/plugins/ after setting MEGAS_BIN.
export const MegasState = async ({ client }) => {
  const binary = process.env.MEGAS_BIN;
  const args = [];
  if (process.env.MEGAS_RUNTIME) args.push("--runtime", process.env.MEGAS_RUNTIME);
  if (process.env.MEGAS_PORT) args.push("--port", process.env.MEGAS_PORT);
  const state = async () => {
    if (!binary) return "Megas state unavailable: MEGAS_BIN has not been configured. Ask the installation agent to select the installed native executable.";
    const child = Bun.spawn([binary, ...args, "status"], { stdout: "pipe", stderr: "pipe" });
    const timer = setTimeout(() => child.kill(), 2000);
    try {
      const text = await new Response(child.stdout).text();
      await child.exited;
      return text.trim().slice(0, 16000) || "Megas state unavailable: check the visible manager and its repair prompt.";
    } catch {
      return "Megas state unavailable: check the visible manager and its repair prompt.";
    } finally {
      clearTimeout(timer);
    }
  };
  await client.app.log({ body: { service: "megas", level: "info", message: "Megas state hook loaded", extra: { binary: binary || "unconfigured" } } });
  return {
    "tool.execute.after": async (_input, output) => {
      const current = await state();
      // V1 OpenCode plugin output. Never replace another plugin's tool content.
      if (typeof output.output === "string") output.output += "\n\nCurrent Megas VM state (not inferred from tool success):\n" + current;
      else await client.app.log({ body: { service: "megas", level: "warn", message: "Unsupported tool result shape; current state not appended", extra: { current } } });
    },
    "experimental.session.compacting": async (_input, output) => {
      output.context.push("Current Megas VM state:\n" + await state());
    },
  };
};
