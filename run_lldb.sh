echo "run" > lldb_cmds
echo "bt" >> lldb_cmds
echo "quit" >> lldb_cmds
lldb ./a.out -s lldb_cmds > lldb_out.txt 2>&1
