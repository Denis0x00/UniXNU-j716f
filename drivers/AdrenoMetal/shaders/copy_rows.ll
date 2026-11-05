; SPDX-License-Identifier: MIT
; Row-by-row copy of a frame onto the scanout where the two pitches differ, for the GPU hand-off (GpuScanoutPresenter.h): pipa's
; compositor surface has rows of 7424 bytes (Turnip's 256-byte linear pitch for 1800 pixels) and the firmware scanout rows of 7200, which
; is not a multiple of 64, so Turnip's copy of the frame's linear texture into the scanout buffer is one 2D blit per row: 2880 blits,
; about 4.3 ms of GPU a frame (2026-10-25, A5 row), most of it the blits' own fixed cost rather than the 41 MB they move.
; Hand-written AIR (the tablet has no Metal compiler), assembled with llvm-as into CopyRowsAir.h by tools/embed-air.py and loaded
; through native_bridge_metal_library_with_air. One thread a 16-byte vector, so that each row of threads writes one row of the
; scanout front to back (the 16x16 tiles of the scale kernel written straight into the scanout cost 35 ms a frame, tb128fu-084):
;   params[0] vectors a row, [1] rows, [2] source row in vectors, [3] target row in vectors.
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"

define void @copy_rows(ptr addrspace(1) noundef readonly "air-buffer-no-alias" %source, ptr addrspace(1) noundef "air-buffer-no-alias" %target, ptr addrspace(2) noundef readonly align 4 "air-buffer-no-alias" %params, <2 x i32> noundef %position) local_unnamed_addr #0 {
entry:
  %x = extractelement <2 x i32> %position, i64 0
  %y = extractelement <2 x i32> %position, i64 1
  %vectors = load i32, ptr addrspace(2) %params, align 4
  %p1 = getelementptr inbounds i32, ptr addrspace(2) %params, i64 1
  %rows = load i32, ptr addrspace(2) %p1, align 4
  %in_x = icmp ult i32 %x, %vectors
  %in_y = icmp ult i32 %y, %rows
  %inside = and i1 %in_x, %in_y
  br i1 %inside, label %body, label %done

body:
  %p2 = getelementptr inbounds i32, ptr addrspace(2) %params, i64 2
  %source_row = load i32, ptr addrspace(2) %p2, align 4
  %p3 = getelementptr inbounds i32, ptr addrspace(2) %params, i64 3
  %target_row = load i32, ptr addrspace(2) %p3, align 4
  ; vector indices in 32 bits (a frame has far fewer than 2^32 vectors; the Adreno has no 64-bit multiply), widened once for the address
  %srow = mul nuw i32 %y, %source_row
  %sj = add nuw i32 %srow, %x
  %si = zext i32 %sj to i64
  %from = getelementptr inbounds <4 x i32>, ptr addrspace(1) %source, i64 %si
  %value = load <4 x i32>, ptr addrspace(1) %from, align 16
  %trow = mul nuw i32 %y, %target_row
  %tj = add nuw i32 %trow, %x
  %ti = zext i32 %tj to i64
  %to = getelementptr inbounds <4 x i32>, ptr addrspace(1) %target, i64 %ti
  store <4 x i32> %value, ptr addrspace(1) %to, align 16
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
!9 = !{ptr @copy_rows, !10, !11}
!10 = !{}
!11 = !{!12, !13, !14, !15}
!12 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint4", !"air.arg_name", !"source"}
!13 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint4", !"air.arg_name", !"target"}
!14 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"params"}
!15 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"position"}
!16 = !{!"air.compile.denorms_disable"}
!17 = !{!"air.compile.fast_math_enable"}
!18 = !{!"hand-written AIR (UniXNU-pipa NativeMetal shaders/copy_rows.ll)"}
!19 = !{i32 2, i32 8, i32 0}
!20 = !{!"Metal", i32 4, i32 0, i32 0}
