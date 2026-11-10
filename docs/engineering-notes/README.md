# Engineering notes

Pipeline knowledge from the port of the monolithic agent to the modular tree, written for whoever works on this
codebase next. They are the notes the porting work ran on, with names of people, machines and internal links
removed — no other content changed.

| Note                                         | Read it when                                                                                     |
| -------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| [`agent.md`](agent.md)                       | working in `Agent/` — module map, the Win32/string/quality rules, build and host tests           |
| [`wire-protocol.md`](wire-protocol.md)       | changing a checkin body, a task request, a task result, or a `TaskType`                          |
| [`adding-a-command.md`](adding-a-command.md) | adding or hardening a command — the validation shape and the places that can silently rewrite it |
| [`teamserver.md`](teamserver.md)             | working in `TeamServer/`, `Client/` or `Trinity.Shared/` — DI, routing, the task-command writers |
| [`honest-results.md`](honest-results.md)     | a feature "succeeded" and did nothing — the defect log behind the artifact-based test suite      |

Two rules run through all of them, and they are the reason the notes exist:

- **Never report success for something that did not happen.** Every command returns a result the agent maps onto
  the task status; a rejected command names the missing field.
- **Assert on artifacts, not on status strings.** Files on disk, rows in the database, a process missing from the
  guest's process table — those are evidence; a `200` or a "done" string is not.

Operator-facing documentation — deployment, prerequisites, troubleshooting, security — is one directory up, in
[`docs/`](../).
