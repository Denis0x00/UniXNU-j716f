; SPDX-License-Identifier: MIT
; Bilinear scale of a BGRA8 frame onto the scanout, for the GPU hand-off (GpuScanoutPresenter.h): a board may have the render server
; draw its frames smaller than the panel (TB128FU: three quarters of 1200x2000), and the GPU scales each one up as it presents it.
; Hand-written AIR (the tablet has no Metal compiler), assembled with llvm-as into ScaleKernelAir.h by tools/embed-air.py and loaded
; through native_bridge_metal_library_with_air. One thread a target pixel, in integer fixed point:
;   params[0] source width, [1] source height, [2] source row in words, [3] target width, [4] target height, [5] target row in words,
;   [6] x step and [7] y step: source texels per target pixel in 16.16.
; The source position of a target pixel's centre is (x + 0.5) * step - 0.5, clamped to the source; each channel is the 8.8-weighted
; mix of the four texels around it, rounded.
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"

define void @scale_bgra(ptr addrspace(1) noundef readonly "air-buffer-no-alias" %source, ptr addrspace(1) noundef "air-buffer-no-alias" %target, ptr addrspace(2) noundef readonly align 4 "air-buffer-no-alias" %params, <2 x i32> noundef %position) local_unnamed_addr #0 {
entry:
  %x = extractelement <2 x i32> %position, i64 0
  %y = extractelement <2 x i32> %position, i64 1
  %p3 = getelementptr inbounds i32, ptr addrspace(2) %params, i64 3
  %target_width = load i32, ptr addrspace(2) %p3, align 4
  %p4 = getelementptr inbounds i32, ptr addrspace(2) %params, i64 4
  %target_height = load i32, ptr addrspace(2) %p4, align 4
  %in_x = icmp ult i32 %x, %target_width
  %in_y = icmp ult i32 %y, %target_height
  %inside = and i1 %in_x, %in_y
  br i1 %inside, label %body, label %done

body:
  %source_width = load i32, ptr addrspace(2) %params, align 4
  %p1 = getelementptr inbounds i32, ptr addrspace(2) %params, i64 1
  %source_height = load i32, ptr addrspace(2) %p1, align 4
  %p2 = getelementptr inbounds i32, ptr addrspace(2) %params, i64 2
  %source_row = load i32, ptr addrspace(2) %p2, align 4
  %p5 = getelementptr inbounds i32, ptr addrspace(2) %params, i64 5
  %target_row = load i32, ptr addrspace(2) %p5, align 4
  %p6 = getelementptr inbounds i32, ptr addrspace(2) %params, i64 6
  %x_step = load i32, ptr addrspace(2) %p6, align 4
  %p7 = getelementptr inbounds i32, ptr addrspace(2) %params, i64 7
  %y_step = load i32, ptr addrspace(2) %p7, align 4

  ; x: the two source columns and the weight of the right one
  %xs_a = mul i32 %x, %x_step
  %x_half = lshr i32 %x_step, 1
  %xs_b = add i32 %xs_a, %x_half
  %xs_c = sub i32 %xs_b, 32768
  %xs_neg = icmp slt i32 %xs_c, 0
  %xs = select i1 %xs_neg, i32 0, i32 %xs_c
  %x_last = sub i32 %source_width, 1
  %x0_a = lshr i32 %xs, 16
  %x0_over = icmp ugt i32 %x0_a, %x_last
  %x0 = select i1 %x0_over, i32 %x_last, i32 %x0_a
  %x1_a = add i32 %x0, 1
  %x1_over = icmp ugt i32 %x1_a, %x_last
  %x1 = select i1 %x1_over, i32 %x_last, i32 %x1_a
  %fx_a = lshr i32 %xs, 8
  %fx = and i32 %fx_a, 255

  ; y: the two source rows and the weight of the lower one
  %ys_a = mul i32 %y, %y_step
  %y_half = lshr i32 %y_step, 1
  %ys_b = add i32 %ys_a, %y_half
  %ys_c = sub i32 %ys_b, 32768
  %ys_neg = icmp slt i32 %ys_c, 0
  %ys = select i1 %ys_neg, i32 0, i32 %ys_c
  %y_last = sub i32 %source_height, 1
  %y0_a = lshr i32 %ys, 16
  %y0_over = icmp ugt i32 %y0_a, %y_last
  %y0 = select i1 %y0_over, i32 %y_last, i32 %y0_a
  %y1_a = add i32 %y0, 1
  %y1_over = icmp ugt i32 %y1_a, %y_last
  %y1 = select i1 %y1_over, i32 %y_last, i32 %y1_a
  %fy_a = lshr i32 %ys, 8
  %fy = and i32 %fy_a, 255

  ; the four texels: word indices in 32 bits (a frame has far fewer than 2^32 words; the Adreno has no 64-bit multiply, and 64-bit
  ; index math made the kernel slower than the copy it replaced, tb128fu-084), widened once for the address
  %row0 = mul nuw i32 %y0, %source_row
  %row1 = mul nuw i32 %y1, %source_row
  %j00 = add nuw i32 %row0, %x0
  %j01 = add nuw i32 %row0, %x1
  %j10 = add nuw i32 %row1, %x0
  %j11 = add nuw i32 %row1, %x1
  %i00 = zext i32 %j00 to i64
  %i01 = zext i32 %j01 to i64
  %i10 = zext i32 %j10 to i64
  %i11 = zext i32 %j11 to i64
  %a00 = getelementptr inbounds i32, ptr addrspace(1) %source, i64 %i00
  %a01 = getelementptr inbounds i32, ptr addrspace(1) %source, i64 %i01
  %a10 = getelementptr inbounds i32, ptr addrspace(1) %source, i64 %i10
  %a11 = getelementptr inbounds i32, ptr addrspace(1) %source, i64 %i11
  %c00 = load i32, ptr addrspace(1) %a00, align 4
  %c01 = load i32, ptr addrspace(1) %a01, align 4
  %c10 = load i32, ptr addrspace(1) %a10, align 4
  %c11 = load i32, ptr addrspace(1) %a11, align 4

  ; channels as four lanes (B, G, R, A)
  %s00 = insertelement <4 x i32> poison, i32 %c00, i64 0
  %v00 = shufflevector <4 x i32> %s00, <4 x i32> poison, <4 x i32> zeroinitializer
  %h00 = lshr <4 x i32> %v00, <i32 0, i32 8, i32 16, i32 24>
  %e00 = and <4 x i32> %h00, splat (i32 255)
  %s01 = insertelement <4 x i32> poison, i32 %c01, i64 0
  %v01 = shufflevector <4 x i32> %s01, <4 x i32> poison, <4 x i32> zeroinitializer
  %h01 = lshr <4 x i32> %v01, <i32 0, i32 8, i32 16, i32 24>
  %e01 = and <4 x i32> %h01, splat (i32 255)
  %s10 = insertelement <4 x i32> poison, i32 %c10, i64 0
  %v10 = shufflevector <4 x i32> %s10, <4 x i32> poison, <4 x i32> zeroinitializer
  %h10 = lshr <4 x i32> %v10, <i32 0, i32 8, i32 16, i32 24>
  %e10 = and <4 x i32> %h10, splat (i32 255)
  %s11 = insertelement <4 x i32> poison, i32 %c11, i64 0
  %v11 = shufflevector <4 x i32> %s11, <4 x i32> poison, <4 x i32> zeroinitializer
  %h11 = lshr <4 x i32> %v11, <i32 0, i32 8, i32 16, i32 24>
  %e11 = and <4 x i32> %h11, splat (i32 255)

  ; weights
  %gx = sub i32 256, %fx
  %wx_s = insertelement <4 x i32> poison, i32 %fx, i64 0
  %wx = shufflevector <4 x i32> %wx_s, <4 x i32> poison, <4 x i32> zeroinitializer
  %wgx_s = insertelement <4 x i32> poison, i32 %gx, i64 0
  %wgx = shufflevector <4 x i32> %wgx_s, <4 x i32> poison, <4 x i32> zeroinitializer
  %gy = sub i32 256, %fy
  %wy_s = insertelement <4 x i32> poison, i32 %fy, i64 0
  %wy = shufflevector <4 x i32> %wy_s, <4 x i32> poison, <4 x i32> zeroinitializer
  %wgy_s = insertelement <4 x i32> poison, i32 %gy, i64 0
  %wgy = shufflevector <4 x i32> %wgy_s, <4 x i32> poison, <4 x i32> zeroinitializer

  ; mix: rows first, then the two rows
  %t0 = mul <4 x i32> %e00, %wgx
  %t1 = mul <4 x i32> %e01, %wx
  %top = add <4 x i32> %t0, %t1
  %b0 = mul <4 x i32> %e10, %wgx
  %b1 = mul <4 x i32> %e11, %wx
  %bottom = add <4 x i32> %b0, %b1
  %m0 = mul <4 x i32> %top, %wgy
  %m1 = mul <4 x i32> %bottom, %wy
  %sum = add <4 x i32> %m0, %m1
  %rounded = add <4 x i32> %sum, splat (i32 32768)
  %mixed = lshr <4 x i32> %rounded, splat (i32 16)

  ; back into one word
  %placed = shl <4 x i32> %mixed, <i32 0, i32 8, i32 16, i32 24>
  %q0 = extractelement <4 x i32> %placed, i64 0
  %q1 = extractelement <4 x i32> %placed, i64 1
  %q2 = extractelement <4 x i32> %placed, i64 2
  %q3 = extractelement <4 x i32> %placed, i64 3
  %q01 = or i32 %q0, %q1
  %q23 = or i32 %q2, %q3
  %pixel = or i32 %q01, %q23

  %trow = mul nuw i32 %y, %target_row
  %tj = add nuw i32 %trow, %x
  %ti = zext i32 %tj to i64
  %out = getelementptr inbounds i32, ptr addrspace(1) %target, i64 %ti
  store i32 %pixel, ptr addrspace(1) %out, align 4
  br label %done

done:
  ret void
}

attributes #0 = { mustprogress nounwind willreturn "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "stack-protector-buffer-size"="8" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8}
!air.kernel = !{!9}
!air.compile_options = !{!16, !17}
!llvm.ident = !{!18}
!air.version = !{!19}
!air.language_version = !{!20}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 27, i32 0]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"frame-pointer", i32 2}
!3 = !{i32 7, !"air.max_device_buffers", i32 31}
!4 = !{i32 7, !"air.max_constant_buffers", i32 31}
!5 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!6 = !{i32 7, !"air.max_textures", i32 128}
!7 = !{i32 7, !"air.max_read_write_textures", i32 8}
!8 = !{i32 7, !"air.max_samplers", i32 16}
!9 = !{ptr @scale_bgra, !10, !11}
!10 = !{}
!11 = !{!12, !13, !14, !15}
!12 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"source"}
!13 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"target"}
!14 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"params"}
!15 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"position"}
!16 = !{!"air.compile.denorms_disable"}
!17 = !{!"air.compile.fast_math_enable"}
!18 = !{!"hand-written AIR (UniXNU-tb128fu NativeMetal shaders/scale_bgra.ll)"}
!19 = !{i32 2, i32 8, i32 0}
!20 = !{!"Metal", i32 4, i32 0, i32 0}
