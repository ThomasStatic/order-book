0000000000000000 <?countResponse@benchmark@order_book@@YAXAEBV?$variant@UOrderAccepted@order_book@@UOrderRejected@2@UTradeExecuted@2@UOrderCancelled@2@UCancelRejected@2@UOrderExpired@2@USelfTradePrevented@2@UStopTriggered@2@UOrderReplaced@2@UReplaceRejected@2@@std@@AEAUWorkloadOutcome@12@@Z>:
subq	$0x38, %rsp
movzbl	0x18(%rcx), %eax
cmpb	$0x1, %al
je	
cmpb	$0x4, %al
je	
cmpb	$0x9, %al
jne	
incq	0x10(%rdx)
movq	%rcx, 0x20(%rsp)
leaq	0x20(%rsp), %rcx
movq	$0x1, 0x28(%rsp)
callq	
		000000000000002c:  IMAGE_REL_AMD64_REL32	?countEvents@benchmark@order_book@@YAXV?$span@$$CBV?$variant@UOrderAccepted@order_book@@UOrderRejected@2@UTradeExecuted@2@UOrderCancelled@2@UCancelRejected@2@UOrderExpired@2@USelfTradePrevented@2@UStopTriggered@2@UOrderReplaced@2@UReplaceRejected@2@@std@@$0?0@std@@AEAUWorkloadOutcome@12@@Z
addq	$0x38, %rsp
retq

Disassembly of section .text$mn:

0000000000000000 <?execute@ANON@benchmark@order_book@@YAXAEAVOrderBook@3@AEBUBehaviorCommand@23@AEAUWorkloadOutcome@23@@Z>:
movq	%rbx, 0x20(%rsp)
pushq	%rdi
subq	$0x1f0, %rsp            # imm = 0x1F0
movq	(%rip), %rax            # 
		0000000000000010:  IMAGE_REL_AMD64_REL32	__security_cookie
xorq	%rsp, %rax
movq	%rax, 0x1e0(%rsp)
movq	%r8, %rbx
movl	(%rdx), %eax
testl	%eax, %eax
jne	
incq	(%r8)
movups	0x4(%rdx), %xmm0
movaps	%xmm0, 0x40(%rsp)
movups	0x14(%rdx), %xmm1
movaps	%xmm1, 0x50(%rsp)
movsd	0x24(%rdx), %xmm0
movsd	%xmm0, 0x60(%rsp)
leaq	0x40(%rsp), %r8
leaq	0x90(%rsp), %rdx
callq	
		000000000000005a:  IMAGE_REL_AMD64_REL32	?submitOrder@OrderBook@order_book@@QEAA?AVSubmissionResult@2@UOrderRequest@2@@Z
nop
leaq	0x90(%rsp), %rcx
callq	
		0000000000000068:  IMAGE_REL_AMD64_REL32	?getStatus@SubmissionResult@order_book@@QEBA?AW4SubmissionStatus@2@XZ
cmpl	$0x1, %eax
jne	
incq	0x10(%rbx)
leaq	0x90(%rsp), %rcx
callq	
		000000000000007e:  IMAGE_REL_AMD64_REL32	?getEvents@SubmissionResult@order_book@@QEBAAEBV?$SmallVector@V?$variant@UOrderAccepted@order_book@@UOrderRejected@2@UTradeExecuted@2@UOrderCancelled@2@UCancelRejected@2@UOrderExpired@2@USelfTradePrevented@2@UStopTriggered@2@UOrderReplaced@2@UReplaceRejected@2@@std@@$05@2@XZ
movq	0x8(%rax), %rdx
movq	(%rax), %rcx
testq	%rcx, %rcx
jne	
leaq	0x18(%rax), %rcx
movq	%rcx, 0x30(%rsp)
movq	%rdx, 0x38(%rsp)
movq	%rbx, %rdx
leaq	0x30(%rsp), %rcx
callq	
		00000000000000a5:  IMAGE_REL_AMD64_REL32	?countEvents@benchmark@order_book@@YAXV?$span@$$CBV?$variant@UOrderAccepted@order_book@@UOrderRejected@2@UTradeExecuted@2@UOrderCancelled@2@UCancelRejected@2@UOrderExpired@2@USelfTradePrevented@2@UStopTriggered@2@UOrderReplaced@2@UReplaceRejected@2@@std@@$0?0@std@@AEAUWorkloadOutcome@12@@Z
nop
leaq	0x120(%rsp), %rcx
callq	
		00000000000000b3:  IMAGE_REL_AMD64_REL32	?releaseHeap@?$SmallVector@V?$variant@UOrderAccepted@order_book@@UOrderRejected@2@UTradeExecuted@2@UOrderCancelled@2@UCancelRejected@2@UOrderExpired@2@USelfTradePrevented@2@UStopTriggered@2@UOrderReplaced@2@UReplaceRejected@2@@std@@$05@order_book@@AEAAXXZ
leaq	0x98(%rsp), %rcx
callq	
		00000000000000c0:  IMAGE_REL_AMD64_REL32	?releaseHeap@?$SmallVector@VTrade@order_book@@$03@order_book@@AEAAXXZ
jmp	
cmpl	$0x2, %eax
jne	
incq	0x38(%r8)
leaq	0x2c(%rdx), %r8
leaq	0x90(%rsp), %rdx
callq	
		00000000000000df:  IMAGE_REL_AMD64_REL32	?replaceOrder@OrderBook@order_book@@QEAA?AVReplaceResult@2@AEBUReplaceRequest@2@@Z
nop
leaq	0x90(%rsp), %rcx
callq	
		00000000000000ed:  IMAGE_REL_AMD64_REL32	?isSuccess@ReplaceResult@order_book@@QEBA_NXZ
testb	%al, %al
jne	
incq	0x10(%rbx)
leaq	0x90(%rsp), %rcx
callq	
		0000000000000102:  IMAGE_REL_AMD64_REL32	?getEvents@ReplaceResult@order_book@@QEBAAEBV?$SmallVector@V?$variant@UOrderAccepted@order_book@@UOrderRejected@2@UTradeExecuted@2@UOrderCancelled@2@UCancelRejected@2@UOrderExpired@2@USelfTradePrevented@2@UStopTriggered@2@UOrderReplaced@2@UReplaceRejected@2@@std@@$05@2@XZ
movq	0x8(%rax), %rdx
movq	(%rax), %rcx
testq	%rcx, %rcx
jne	
leaq	0x18(%rax), %rcx
movq	%rcx, 0x30(%rsp)
movq	%rdx, 0x38(%rsp)
movq	%rbx, %rdx
leaq	0x30(%rsp), %rcx
callq	
		0000000000000129:  IMAGE_REL_AMD64_REL32	?countEvents@benchmark@order_book@@YAXV?$span@$$CBV?$variant@UOrderAccepted@order_book@@UOrderRejected@2@UTradeExecuted@2@UOrderCancelled@2@UCancelRejected@2@UOrderExpired@2@USelfTradePrevented@2@UStopTriggered@2@UOrderReplaced@2@UReplaceRejected@2@@std@@$0?0@std@@AEAUWorkloadOutcome@12@@Z
nop
leaq	0x118(%rsp), %rcx
callq	
		0000000000000137:  IMAGE_REL_AMD64_REL32	?releaseHeap@?$SmallVector@V?$variant@UOrderAccepted@order_book@@UOrderRejected@2@UTradeExecuted@2@UOrderCancelled@2@UCancelRejected@2@UOrderExpired@2@USelfTradePrevented@2@UStopTriggered@2@UOrderReplaced@2@UReplaceRejected@2@@std@@$05@order_book@@AEAAXXZ
leaq	0xa0(%rsp), %rcx
callq	
		0000000000000144:  IMAGE_REL_AMD64_REL32	?releaseHeap@?$SmallVector@VTrade@order_book@@$03@order_book@@AEAAXXZ
jmp	
incq	0x8(%r8)
movl	0x4(%rdx), %r8d
leaq	0x40(%rsp), %rdx
callq	
		0000000000000158:  IMAGE_REL_AMD64_REL32	?cancelOrder@OrderBook@order_book@@QEAA?AVCancellationResult@2@I@Z
nop
movq	%rax, %rcx
callq	
		0000000000000161:  IMAGE_REL_AMD64_REL32	?isSuccess@CancellationResult@order_book@@QEBA_NXZ
movzbl	%al, %edi
movq	0x50(%rsp), %r8
testq	%r8, %r8
je	
imulq	$0x1c, 0x60(%rsp), %rdx
movq	%r8, %rcx
cmpq	$0x1000, %rdx           # imm = 0x1000
jb	
addq	$0x27, %rdx
movq	-0x8(%r8), %r8
subq	%r8, %rcx
subq	$0x8, %rcx
cmpq	$0x1f, %rcx
jbe	
movq	$0x0, 0x20(%rsp)
xorl	%r9d, %r9d
xorl	%r8d, %r8d
xorl	%edx, %edx
xorl	%ecx, %ecx
callq	*(%rip)                 # 
		00000000000001ae:  IMAGE_REL_AMD64_REL32	__imp__invoke_watson
int3
movq	%r8, %rcx
callq	
		00000000000001b7:  IMAGE_REL_AMD64_REL32	??3@YAXPEAX_K@Z
testb	%dil, %dil
jne	
incq	0x10(%rbx)
movq	0x1e0(%rsp), %rcx
xorq	%rsp, %rcx
callq	
		00000000000001d0:  IMAGE_REL_AMD64_REL32	__security_check_cookie
movq	0x218(%rsp), %rbx
addq	$0x1f0, %rsp            # imm = 0x1F0
popq	%rdi
retq

Disassembly of section .text$x:

0000000000000000 <?execute@ANON@benchmark@order_book@@YAXAEAVOrderBook@3@AEBUBenchmarkCommand@23@AEAUWorkloadOutcome@23@@Z>:
pushq	%rbx
pushq	%rsi
pushq	%rdi
subq	$0x230, %rsp            # imm = 0x230
movq	(%rip), %rax            # 
		000000000000000e:  IMAGE_REL_AMD64_REL32	__security_cookie
xorq	%rsp, %rax
movq	%rax, 0x220(%rsp)
movq	%r8, %rbx
movq	%rcx, %rdi
cmpl	$0x0, (%rdx)
jne	
incq	(%r8)
movl	$0x0, 0x30(%rsp)
movl	0x8(%rdx), %eax
movl	%eax, 0x28(%rsp)
movl	$0x1, 0x20(%rsp)
movl	0x10(%rdx), %r9d
movl	0xc(%rdx), %r8d
movl	0x4(%rdx), %edx
leaq	0x60(%rsp), %rcx
callq	
		0000000000000057:  IMAGE_REL_AMD64_REL32	??0Order@order_book@@QEAA@IIIIW4Side@1@W4OrderType@1@@Z
movups	(%rax), %xmm0
movaps	%xmm0, 0x40(%rsp)
movups	0x10(%rax), %xmm1
movaps	%xmm1, 0x50(%rsp)
leaq	0x40(%rsp), %r8
leaq	0xd0(%rsp), %rdx
movq	%rdi, %rcx
callq	
		000000000000007d:  IMAGE_REL_AMD64_REL32	?submitOrder@OrderBook@order_book@@QEAA?AVSubmissionResult@2@VOrder@2@@Z
nop
leaq	0xd0(%rsp), %rcx
callq	
		000000000000008b:  IMAGE_REL_AMD64_REL32	?getStatus@SubmissionResult@order_book@@QEBA?AW4SubmissionStatus@2@XZ
cmpl	$0x1, %eax
jne	
incq	0x10(%rbx)
leaq	0xd0(%rsp), %rcx
callq	
		00000000000000a1:  IMAGE_REL_AMD64_REL32	?getGeneratedTrades@SubmissionResult@order_book@@QEBAAEBV?$SmallVector@VTrade@order_book@@$03@2@XZ
movq	(%rax), %rdx
leaq	0x18(%rax), %r8
testq	%rdx, %rdx
je	
movq	%rdx, %r8
movq	%rdx, %rdi
jmp	
movq	%r8, %rdi
movq	%r8, %rdx
movq	0x8(%rax), %rax
leaq	(%rax,%rax,2), %rcx
leaq	(%rdx,%rcx,8), %rsi
cmpq	%rsi, %r8
je	
incq	0x18(%rbx)
movq	%rdi, %rcx
callq	
		00000000000000d8:  IMAGE_REL_AMD64_REL32	?getExecutionQuantity@Trade@order_book@@QEBAIXZ
movl	%eax, %eax
addq	%rax, 0x20(%rbx)
addq	$0x18, %rdi
cmpq	%rsi, %rdi
jne	
leaq	0x160(%rsp), %rcx
callq	
		00000000000000f4:  IMAGE_REL_AMD64_REL32	?releaseHeap@?$SmallVector@V?$variant@UOrderAccepted@order_book@@UOrderRejected@2@UTradeExecuted@2@UOrderCancelled@2@UCancelRejected@2@UOrderExpired@2@USelfTradePrevented@2@UStopTriggered@2@UOrderReplaced@2@UReplaceRejected@2@@std@@$05@order_book@@AEAAXXZ
leaq	0xd8(%rsp), %rcx
callq	
		0000000000000101:  IMAGE_REL_AMD64_REL32	?releaseHeap@?$SmallVector@VTrade@order_book@@$03@order_book@@AEAAXXZ
jmp	
incq	0x8(%r8)
movl	0x4(%rdx), %r8d
leaq	0x80(%rsp), %rdx
callq	
		000000000000011b:  IMAGE_REL_AMD64_REL32	?cancelOrder@OrderBook@order_book@@QEAA?AVCancellationResult@2@I@Z
nop
movq	%rax, %rcx
callq	
		0000000000000124:  IMAGE_REL_AMD64_REL32	?isSuccess@CancellationResult@order_book@@QEBA_NXZ
movzbl	%al, %edi
movq	0x90(%rsp), %r8
testq	%r8, %r8
je	
imulq	$0x1c, 0xa0(%rsp), %rdx
movq	%r8, %rcx
cmpq	$0x1000, %rdx           # imm = 0x1000
jb	
addq	$0x27, %rdx
movq	-0x8(%r8), %r8
subq	%r8, %rcx
subq	$0x8, %rcx
cmpq	$0x1f, %rcx
jbe	
movq	$0x0, 0x20(%rsp)
xorl	%r9d, %r9d
xorl	%r8d, %r8d
xorl	%edx, %edx
xorl	%ecx, %ecx
callq	*(%rip)                 # 
		0000000000000177:  IMAGE_REL_AMD64_REL32	__imp__invoke_watson
int3
movq	%r8, %rcx
callq	
		0000000000000180:  IMAGE_REL_AMD64_REL32	??3@YAXPEAX_K@Z
testb	%dil, %dil
jne	
incq	0x10(%rbx)
movq	0x220(%rsp), %rcx
xorq	%rsp, %rcx
callq	
		0000000000000199:  IMAGE_REL_AMD64_REL32	__security_check_cookie
addq	$0x230, %rsp            # imm = 0x230
popq	%rdi
popq	%rsi
popq	%rbx
retq

Disassembly of section .text$x:

