## try to capture nginx accept through bpftrace

```bash
bpftrace -e 'tracepoint:syscalls:sys_enter_accept4 { printf("accept\n"); }'
```

## print process comm

## print only interested process
append condition before calling printf

/ comm == "networkio" /

event
/condition/
'
{
action
}

'

## use a .bt script to setup bpftrace command





