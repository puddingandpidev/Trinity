//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using System;
using Trinity.Shared.Interfaces;

namespace Trinity.Shared.DTOs.Command
{
    /// <summary>
    /// Persistence via Windows task scheduling (KAN-110, spec 11.1.1.1). The agent drops its own
    /// payload at <see cref="Path"/> and registers it as a logon-triggered task named
    /// <see cref="TaskName"/>, running as <see cref="Username"/> when one is supplied.
    /// </summary>
    public class WinTaskDTO : ICommand
    {
        public string Path { get; set; } = String.Empty;

        public string Username { get; set; } = String.Empty;

        public string TaskName { get; set; } = String.Empty;
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
