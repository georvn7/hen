mov r0 5
call inc
halt r0
inc:
mov r1 1
add r0 r1
ret
