// ========================================== { START OF FILE } ========================================== //
using System;

namespace Trinity.Shared.DTOs.Accounts
{
    /// <summary>
    /// An operator as the API returns it: identity and last sign-in, never credential material.
    /// </summary>
    /// <remarks>
    /// The list and detail routes used to return the persistence model
    /// (<see cref="Trinity.Shared.Models.Operator"/>), whose <c>Password</c> property is written out
    /// by the default JSON serializer — so any authenticated operator could read every operator's
    /// password (and hash) from <c>GET /api/v1/operators</c>. Requests still accept the model,
    /// because create and update need the incoming password; responses use this type.
    /// </remarks>
    public record OperatorSummary(int ID, string Username, DateTime? LastLogin);

    /// <summary>An admin account as the API returns it: identity only, never credential material.</summary>
    public record AdminSummary(int ID, string Username, DateTime LastLogin);
}
// ========================================== { END OF FILE } ========================================== //
