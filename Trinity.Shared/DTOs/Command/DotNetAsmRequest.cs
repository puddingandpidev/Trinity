//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using System;
using Trinity.Shared.Interfaces;

namespace Trinity.Shared.DTOs.Command
{
    /// <summary>
    /// The command a NET_ASM task puts on the wire. <see cref="CommandService"/> reads the tool at
    /// the request's <c>AssemblyPath</c>, base64-encodes it into <see cref="Assembly"/>, and queues
    /// this instead — the agent executes the decoded blob in memory, so it never needs a path.
    /// </summary>
    public class DotNetAsmRequest : ICommand
    {
        public string Assembly { get; set; } = String.Empty;

        public string Arguments { get; set; } = String.Empty;
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
