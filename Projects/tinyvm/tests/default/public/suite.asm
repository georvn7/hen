mov r0 3
mov r1 0
mov r2 1
loop:
add r1 r0
sub r0 r2
jz r0 done
jmp loop
done:
halt r1

; memory tail unreachable after halt
