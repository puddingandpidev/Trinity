//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using System;
using Trinity.Shared.Interfaces;

namespace Trinity.Shared.DTOs.Command
{
    /// <summary>
    /// Token impersonation task (KAN-45).
    /// Ref: MITRE T1134.001 (Token Impersonation/Theft)
    /// https://attack.mitre.org/techniques/T1134/001/
    /// </summary>
    public class TokenDTO : ICommand
    {
        /// <summary>One of: make, steal, rev2self.</summary>
        public string Operation { get; set; } = String.Empty;

        /// <summary>Account name for the "make" operation.</summary>
        public string Username { get; set; } = String.Empty;

        /// <summary>Password for the "make" operation.</summary>
        public string Password { get; set; } = String.Empty;

        /// <summary>Target process identifier for the "steal" operation.</summary>
        public int PID { get; set; } = default;
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
