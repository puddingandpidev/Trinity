//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
namespace Trinity.Shared.DTOs.Command
{
    /// <summary>
    /// The on-wire command a chunked download sends to the agent (the task's Command override).
    /// Typed so the document cannot lose <c>TransferID</c> or the full destination <c>Path</c>; the
    /// agent derives the campaign id from <c>TransferID</c> and writes <c>Data</c> at <c>Offset</c>.
    /// </summary>
    public record DownloadChunkPayload(
        string Path,
        string? TransferID,
        string Data,
        long Offset,
        long TotalBytes,
        bool Final
    );
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
