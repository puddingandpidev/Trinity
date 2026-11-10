using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace TeamServer.Migrations
{
    /// <inheritdoc />
    public partial class add_token_keylogger_task_types : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.AlterDatabase()
                .Annotation("Npgsql:Enum:status", "dispatched,error,more_to_follow,no_flag,no_more_to_follow,queued,running,success")
                .Annotation("Npgsql:Enum:taskCode", "cancel_download,download,kill_process,no_code,run,run_as,run_u,sleep,spawn_to,update_hosts")
                .Annotation("Npgsql:Enum:taskType", "download,escalate,get_uid,intial_checkin,keylogger,kill,net_asm,nop,object_file,powershell,process,set,shell,token,upload")
                .OldAnnotation("Npgsql:Enum:status", "dispatched,error,more_to_follow,no_flag,no_more_to_follow,queued,running,success")
                .OldAnnotation("Npgsql:Enum:taskCode", "cancel_download,download,kill_process,no_code,run,run_as,run_u,sleep,spawn_to,update_hosts")
                .OldAnnotation("Npgsql:Enum:taskType", "download,escalate,get_uid,intial_checkin,kill,net_asm,nop,object_file,powershell,process,set,shell,upload");
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.AlterDatabase()
                .Annotation("Npgsql:Enum:status", "dispatched,error,more_to_follow,no_flag,no_more_to_follow,queued,running,success")
                .Annotation("Npgsql:Enum:taskCode", "cancel_download,download,kill_process,no_code,run,run_as,run_u,sleep,spawn_to,update_hosts")
                .Annotation("Npgsql:Enum:taskType", "download,escalate,get_uid,intial_checkin,kill,net_asm,nop,object_file,powershell,process,set,shell,upload")
                .OldAnnotation("Npgsql:Enum:status", "dispatched,error,more_to_follow,no_flag,no_more_to_follow,queued,running,success")
                .OldAnnotation("Npgsql:Enum:taskCode", "cancel_download,download,kill_process,no_code,run,run_as,run_u,sleep,spawn_to,update_hosts")
                .OldAnnotation("Npgsql:Enum:taskType", "download,escalate,get_uid,intial_checkin,keylogger,kill,net_asm,nop,object_file,powershell,process,set,shell,token,upload");
        }
    }
}
