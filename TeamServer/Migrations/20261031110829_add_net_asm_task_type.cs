using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace TeamServer.Migrations
{
    /// <inheritdoc />
    public partial class add_net_asm_task_type : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            // Npgsql does not diff Postgres enums; add the new 'net_asm' value explicitly.
            migrationBuilder.Sql("ALTER TYPE \"taskType\" ADD VALUE IF NOT EXISTS 'net_asm';");
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            // PostgreSQL does not support dropping enum values; nothing to do.
        }
    }
}
