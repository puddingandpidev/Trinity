//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using Microsoft.EntityFrameworkCore;
using System.Text.Json;
using TeamServer.Data;
using Trinity.Shared.DTOs.Listener;
using Trinity.Shared.Errors;
using Trinity.Shared.Interfaces;
using Trinity.Shared.Models;
using Trinity.Shared.Results;

namespace TeamServer.Repositories
{
    public class ListenerRepository : RepositoryBase<Listener>, IListenerRepository
    {
        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Default constructor.
        /// </summary>
        /// <param name="context"></param>
        public ListenerRepository(Context context) : base(context)
        {
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Adds a Listener to the context asynchronously and returns its generated ID.
        /// </summary>
        /// <remarks>The entity is added to the DbContext; changes are not persisted until
        /// SaveChangesAsync is called.</remarks>
        /// <param name="listener">Listener to add.</param>
        /// <returns>A task that represents the asynchronous operation. The task result contains the ID of the added Listener.</returns>
        public async System.Threading.Tasks.Task AddListenerAsync(Listener listener)
        {
            await _dbSet.AddAsync(listener);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Retrieves all listeners and projects them into ListenerResponse instances containing the listener ID, name,
        /// and protocol name.
        /// </summary>
        /// <returns>A task that represents the asynchronous operation. The task result contains a list of ListenerResponse
        /// objects.</returns>
        public async Task<List<ListenerResponse>> GetListenersAsync()
        {
            var listeners = await _dbSet
                .Include(l => l.Protocol)
                .ToListAsync();

            // Both columns exist server-side already; they were simply not part of this DTO.
            // Reported additively (coordinator call 2026-11-06, owner may override) rather
            // than fabricating them client-side.
            var agentCounts = await _context.Agents
                .GroupBy(a => a.ListenerID)
                .Select(g => new { ListenerID = g.Key, Count = g.Count() })
                .ToDictionaryAsync(x => x.ListenerID, x => x.Count);

            return listeners
                .Select(l => new ListenerResponse(
                    l.ID,
                    l.Name,
                    l.Protocol.Name,
                    DescribeBind(l.Config),
                    agentCounts.TryGetValue(l.ID, out var count) ? count : 0))
                .ToList();
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Renders the listener's stored config as "host:port" for the list screen.
        /// </summary>
        /// <remarks>The config is a persisted JsonDocument written by the factory in PascalCase;
        /// both spellings are accepted because older rows may differ, and a malformed row must not
        /// take the whole listing down.</remarks>
        /// <param name="config">The listener's stored configuration document.</param>
        /// <returns>"host:port" when a port is present, otherwise null.</returns>
        private static string? DescribeBind(JsonDocument config)
        {
            try
            {
                var root = config.RootElement;
                string? host = null;
                foreach (var key in new[] { "Hosts", "hosts" })
                {
                    if (root.TryGetProperty(key, out var hosts)
                        && hosts.ValueKind == JsonValueKind.Array
                        && hosts.GetArrayLength() > 0)
                    {
                        host = hosts[0].GetString();
                        break;
                    }
                }

                int? port = null;
                foreach (var key in new[] { "BindPort", "bindPort", "C2Port", "c2Port" })
                {
                    if (root.TryGetProperty(key, out var value) && value.TryGetInt32(out var parsed))
                    {
                        port = parsed;
                        break;
                    }
                }

                if (port is null)
                {
                    return null;
                }

                string bindHost = string.IsNullOrWhiteSpace(host) ? "0.0.0.0" : host;
                return $"{bindHost}:{port}";
            }
            catch (Exception)
            {
                return null;
            }
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Asynchronously retrieves the Listener with the specified identifier.
        /// </summary>
        /// <param name="ID">The identifier of the Listener to retrieve.</param>
        /// <returns>A Task whose result is the Listener with the specified identifier, or null if no matching entity is found.</returns>
        public async Task<Listener> GetListenerAsync(int ID)
        {
            var listener = await _dbSet.FindAsync(ID);
            return listener;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Removes the specified listener from the underlying DbSet.
        /// </summary>
        /// <remarks>Marks the entity for deletion in the context; the removal is applied when SaveChanges
        /// is called.</remarks>
        /// <param name="listener">The listener to remove.</param>
        public void DeleteListener(Listener listener)
        {
            _dbSet.Remove(listener);
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//