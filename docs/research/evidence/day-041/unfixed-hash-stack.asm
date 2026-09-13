
/work/day041-76d03ac/build/x86_64/lib/crypto/sha256.o:     file format elf64-x86-64


Disassembly of section .text:

0000000000000120 <sha256_compress>:
 120:	55                   	push   %rbp
 121:	48 89 e5             	mov    %rsp,%rbp
 124:	41 57                	push   %r15
 126:	41 56                	push   %r14
 128:	41 55                	push   %r13
 12a:	41 54                	push   %r12
 12c:	53                   	push   %rbx
 12d:	48 81 ec a8 00 00 00 	sub    $0xa8,%rsp
 134:	31 c0                	xor    %eax,%eax
 136:	66 2e 0f 1f 84 00 00 	cs nopw 0x0(%rax,%rax,1)
 13d:	00 00 00 
 140:	8b 0c 86             	mov    (%rsi,%rax,4),%ecx
 143:	0f c9                	bswap  %ecx
 145:	89 8c 85 b0 fe ff ff 	mov    %ecx,-0x150(%rbp,%rax,4)
 14c:	48 83 c0 01          	add    $0x1,%rax
 150:	48 83 f8 10          	cmp    $0x10,%rax
 154:	75 ea                	jne    140 <sha256_compress+0x20>
 156:	b8 10 00 00 00       	mov    $0x10,%eax
 15b:	44 8b 85 b0 fe ff ff 	mov    -0x150(%rbp),%r8d
 162:	45 89 c1             	mov    %r8d,%r9d
 165:	66 2e 0f 1f 84 00 00 	cs nopw 0x0(%rax,%rax,1)
 16c:	00 00 00 
 16f:	90                   	nop
 170:	8b b4 85 a8 fe ff ff 	mov    -0x158(%rbp,%rax,4),%esi
 177:	89 f3                	mov    %esi,%ebx
 179:	c1 c3 0f             	rol    $0xf,%ebx
 17c:	44 8b 94 85 74 fe ff 	mov    -0x18c(%rbp,%rax,4),%r10d
 183:	ff 
 184:	89 f2                	mov    %esi,%edx
 186:	c1 c2 0d             	rol    $0xd,%edx
 189:	31 da                	xor    %ebx,%edx
 18b:	c1 ee 0a             	shr    $0xa,%esi
 18e:	31 d6                	xor    %edx,%esi
 190:	03 b4 85 94 fe ff ff 	add    -0x16c(%rbp,%rax,4),%esi
 197:	44 89 d2             	mov    %r10d,%edx
 19a:	44 89 d1             	mov    %r10d,%ecx
 19d:	44 89 d3             	mov    %r10d,%ebx
 1a0:	c1 c3 19             	rol    $0x19,%ebx
 1a3:	c1 c2 0e             	rol    $0xe,%edx
 1a6:	44 01 ce             	add    %r9d,%esi
 1a9:	45 89 d1             	mov    %r10d,%r9d
 1ac:	31 da                	xor    %ebx,%edx
 1ae:	c1 e9 03             	shr    $0x3,%ecx
 1b1:	31 d1                	xor    %edx,%ecx
 1b3:	01 f1                	add    %esi,%ecx
 1b5:	89 8c 85 b0 fe ff ff 	mov    %ecx,-0x150(%rbp,%rax,4)
 1bc:	48 83 c0 01          	add    $0x1,%rax
 1c0:	48 83 f8 40          	cmp    $0x40,%rax
 1c4:	75 aa                	jne    170 <sha256_compress+0x50>
 1c6:	44 8b 3f             	mov    (%rdi),%r15d
 1c9:	44 8b 67 04          	mov    0x4(%rdi),%r12d
 1cd:	44 8b 5f 08          	mov    0x8(%rdi),%r11d
 1d1:	8b 5f 0c             	mov    0xc(%rdi),%ebx
 1d4:	44 8b 77 10          	mov    0x10(%rdi),%r14d
 1d8:	8b 57 14             	mov    0x14(%rdi),%edx
 1db:	44 8b 6f 18          	mov    0x18(%rdi),%r13d
 1df:	8b 47 1c             	mov    0x1c(%rdi),%eax
 1e2:	41 ba 04 00 00 00    	mov    $0x4,%r10d
 1e8:	44 89 7d d4          	mov    %r15d,-0x2c(%rbp)
 1ec:	44 89 65 d0          	mov    %r12d,-0x30(%rbp)
 1f0:	89 45 b8             	mov    %eax,-0x48(%rbp)
 1f3:	44 89 6d bc          	mov    %r13d,-0x44(%rbp)
 1f7:	89 55 c0             	mov    %edx,-0x40(%rbp)
 1fa:	44 89 75 c4          	mov    %r14d,-0x3c(%rbp)
 1fe:	89 5d c8             	mov    %ebx,-0x38(%rbp)
 201:	44 89 5d cc          	mov    %r11d,-0x34(%rbp)
 205:	66 2e 0f 1f 84 00 00 	cs nopw 0x0(%rax,%rax,1)
 20c:	00 00 00 
 20f:	90                   	nop
 210:	44 89 f1             	mov    %r14d,%ecx
 213:	c1 c1 1a             	rol    $0x1a,%ecx
 216:	45 89 e1             	mov    %r12d,%r9d
 219:	45 89 fc             	mov    %r15d,%r12d
 21c:	44 89 f6             	mov    %r14d,%esi
 21f:	c1 c6 15             	rol    $0x15,%esi
 222:	31 ce                	xor    %ecx,%esi
 224:	44 89 f1             	mov    %r14d,%ecx
 227:	c1 c1 07             	rol    $0x7,%ecx
 22a:	31 f1                	xor    %esi,%ecx
 22c:	89 d6                	mov    %edx,%esi
 22e:	44 21 f6             	and    %r14d,%esi
 231:	01 ce                	add    %ecx,%esi
 233:	01 c6                	add    %eax,%esi
 235:	44 89 f0             	mov    %r14d,%eax
 238:	f7 d0                	not    %eax
 23a:	44 21 e8             	and    %r13d,%eax
 23d:	01 f0                	add    %esi,%eax
 23f:	41 03 82 00 00 00 00 	add    0x0(%r10),%eax
 246:	44 01 c0             	add    %r8d,%eax
 249:	44 89 f9             	mov    %r15d,%ecx
 24c:	c1 c1 1e             	rol    $0x1e,%ecx
 24f:	44 89 fe             	mov    %r15d,%esi
 252:	c1 c6 13             	rol    $0x13,%esi
 255:	31 ce                	xor    %ecx,%esi
 257:	44 89 f9             	mov    %r15d,%ecx
 25a:	c1 c1 0a             	rol    $0xa,%ecx
 25d:	31 f1                	xor    %esi,%ecx
 25f:	44 89 ce             	mov    %r9d,%esi
 262:	44 31 de             	xor    %r11d,%esi
 265:	44 21 fe             	and    %r15d,%esi
 268:	45 89 cf             	mov    %r9d,%r15d
 26b:	45 21 df             	and    %r11d,%r15d
 26e:	41 31 f7             	xor    %esi,%r15d
 271:	41 01 cf             	add    %ecx,%r15d
 274:	01 c3                	add    %eax,%ebx
 276:	41 01 c7             	add    %eax,%r15d
 279:	49 81 fa 00 01 00 00 	cmp    $0x100,%r10
 280:	74 23                	je     2a5 <sha256_compress+0x185>
 282:	46 8b 84 15 b0 fe ff 	mov    -0x150(%rbp,%r10,1),%r8d
 289:	ff 
 28a:	49 83 c2 04          	add    $0x4,%r10
 28e:	44 89 e8             	mov    %r13d,%eax
 291:	41 89 d5             	mov    %edx,%r13d
 294:	44 89 f2             	mov    %r14d,%edx
 297:	41 89 de             	mov    %ebx,%r14d
 29a:	44 89 db             	mov    %r11d,%ebx
 29d:	45 89 cb             	mov    %r9d,%r11d
 2a0:	e9 6b ff ff ff       	jmp    210 <sha256_compress+0xf0>
 2a5:	44 03 7d d4          	add    -0x2c(%rbp),%r15d
 2a9:	44 89 3f             	mov    %r15d,(%rdi)
 2ac:	44 03 65 d0          	add    -0x30(%rbp),%r12d
 2b0:	44 89 67 04          	mov    %r12d,0x4(%rdi)
 2b4:	44 03 4d cc          	add    -0x34(%rbp),%r9d
 2b8:	44 89 4f 08          	mov    %r9d,0x8(%rdi)
 2bc:	44 03 5d c8          	add    -0x38(%rbp),%r11d
 2c0:	44 89 5f 0c          	mov    %r11d,0xc(%rdi)
 2c4:	03 5d c4             	add    -0x3c(%rbp),%ebx
 2c7:	89 5f 10             	mov    %ebx,0x10(%rdi)
 2ca:	44 03 75 c0          	add    -0x40(%rbp),%r14d
 2ce:	44 89 77 14          	mov    %r14d,0x14(%rdi)
 2d2:	03 55 bc             	add    -0x44(%rbp),%edx
 2d5:	89 57 18             	mov    %edx,0x18(%rdi)
 2d8:	44 03 6d b8          	add    -0x48(%rbp),%r13d
 2dc:	44 89 6f 1c          	mov    %r13d,0x1c(%rdi)
 2e0:	48 81 c4 a8 00 00 00 	add    $0xa8,%rsp
 2e7:	5b                   	pop    %rbx
 2e8:	41 5c                	pop    %r12
 2ea:	41 5d                	pop    %r13
 2ec:	41 5e                	pop    %r14
 2ee:	41 5f                	pop    %r15
 2f0:	5d                   	pop    %rbp
 2f1:	c3                   	ret
