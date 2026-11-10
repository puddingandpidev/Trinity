//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
namespace Trinity.Shared.DTOs.Command
{
    /// <summary>
    /// The stored command a chunked download replaces <c>Task.Command</c> with. Typed so the
    /// rewrite cannot drop a wire key — <c>TransferID</c> was dropped here before and the agent
    /// then refused the task. The keys are frozen: <c>Path</c>, <c>TransferID</c>, <c>Offset</c>,
    /// <c>TotalBytes</c>, <c>Final</c>.
    /// </summary>
    public record DownloadChunkState(
        string Path,
        string? TransferID,
        long Offset,
        long TotalBytes,
        bool Final
    );
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
