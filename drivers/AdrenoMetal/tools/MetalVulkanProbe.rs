// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline consumer of a pinned translator's public API; never opens a GPU.
use metal2vulkan::device::{Device, DeviceDescription, DevicePolicy, SamplingPolicy};
use metal2vulkan::library::{FunctionStage, Library};
use metal2vulkan::metal::PixelFormat;
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, ComputePipelineDescriptor, FunctionDescriptor,
    RenderPipelineDescriptor, StageModule, StageSource, VertexDescriptor,
};
use metal2vulkan::Budget;
use serde_json::{json, Value};
use std::{collections::BTreeMap, env, fs, process};

fn stage(module: &StageModule) -> Value {
    json!({
        "stage": format!("{:?}", module.stage),
        "entry_point": module.entry_point,
        "spirv": &*module.spirv,
        "precision_notes": module.precision_notes.iter().map(|n| format!("{n:?}")).collect::<Vec<_>>(),
        "required_subgroup_size": module.required_subgroup_size,
        "specialization": format!("{:?}", module.specialization),
        "constant_blob_bytes": module.constant_blob.as_ref().map_or(0, |b| b.len()),
    })
}

fn run(args: &[String]) -> Result<Value, Box<dyn std::error::Error>> {
    if args.len() == 2 && args[0] == "--list" {
        let library = Library::parse(&fs::read(&args[1])?)?;
        return Ok(json!({"functions": library.functions().map(|f| json!({
            "name": f.name, "stage": format!("{:?}", f.stage)
        })).collect::<Vec<_>>(), "gpu_execution": false}));
    }
    let mut args = args;
    let mut vertex_descriptor = None;
    let mut color_attachments = None;
    while args.first().is_some_and(|v| v.starts_with("--")) {
        let path = args.get(1).ok_or("missing descriptor path")?;
        match args[0].as_str() {
            "--vertex-descriptor" if vertex_descriptor.is_none() => {
                vertex_descriptor = Some(serde_json::from_slice::<VertexDescriptor>(&fs::read(
                    path,
                )?)?);
            }
            "--color-attachments" if color_attachments.is_none() => {
                let colors =
                    serde_json::from_slice::<BTreeMap<usize, PixelFormat>>(&fs::read(path)?)?;
                if colors.is_empty() || colors.keys().any(|index| *index >= 8) {
                    return Err("color attachments require indices in 0..8".into());
                }
                color_attachments = Some(colors);
            }
            _ => return Err("unknown or duplicate descriptor option".into()),
        }
        args = &args[2..];
    }
    if args.len() != 3 && args.len() != 5 {
        return Err(
            "expected --list INPUT, or PROFILE INPUT FUNCTION [FRAGMENT_INPUT FUNCTION]".into(),
        );
    }
    let description = DeviceDescription::from_khronos_json(&fs::read_to_string(&args[0])?, None)?;
    let mut policy = DevicePolicy::default().exact_answers();
    // exact_answers deliberately keeps sampling unchanged in this upstream revision.
    policy.sampling = SamplingPolicy::Exact;
    let device = Device::new(description, policy)?;
    let library = Library::parse(&fs::read(&args[1])?)?;
    let function = library.function(&args[2])?;
    let budget = Budget::standard();
    let (stages, layout, interface, dependencies, requirements) = match function.info().stage {
        FunctionStage::Kernel if args.len() == 3 => {
            if vertex_descriptor.is_some() || color_attachments.is_some() {
                return Err("compute pipeline does not take render descriptors".into());
            }
            let descriptor = ComputePipelineDescriptor::new(StageSource::Function(
                FunctionDescriptor::new(function),
            ));
            let p = device.compile_compute_pipeline(&descriptor, &budget)?;
            (
                vec![stage(&p.stage)],
                format!("{:?}", p.layout),
                format!("{:?}", p.interface),
                format!("{:?}", p.dependencies),
                format!("{:?}", p.requirements),
            )
        }
        FunctionStage::Vertex => {
            let mut descriptor = RenderPipelineDescriptor::new(StageSource::Function(
                FunctionDescriptor::new(function),
            ));
            descriptor.vertex_descriptor = vertex_descriptor;
            if args.len() == 5 {
                let fragment_library = Library::parse(&fs::read(&args[3])?)?;
                let fragment = fragment_library.function(&args[4])?;
                if fragment.info().stage != FunctionStage::Fragment {
                    return Err("fragment input is not a fragment function".into());
                }
                descriptor.fragment =
                    Some(StageSource::Function(FunctionDescriptor::new(fragment)));
                let colors = color_attachments
                    .as_ref()
                    .ok_or("fragment translation requires explicit color attachments")?;
                for (&index, &format) in colors {
                    descriptor.color_attachments[index] =
                        Some(ColorAttachmentDescriptor::new(format));
                }
            } else if color_attachments.is_some() {
                return Err("color attachments require a fragment function in this probe".into());
            }
            let p = device.compile_render_pipeline(&descriptor, &budget)?;
            (
                p.stages.iter().map(stage).collect(),
                format!("{:?}", p.layout),
                format!("{:?}", p.interface),
                format!("{:?}", p.dependencies),
                format!("{:?}", p.requirements),
            )
        }
        _ => {
            return Err(
                "stage needs a matching pipeline descriptor; standalone fragment refused".into(),
            )
        }
    };
    Ok(json!({
        "stages": stages, "layout": layout, "interface": interface,
        "dependencies": dependencies, "requirements": requirements,
        "policy": "exact_answers+exact_sampling",
        "validation_envs": device.profile().spirv_validation().target_envs,
        "validation_relaxations": device.profile().spirv_validation().relaxations,
        "profile_is_hardware_observation": false,
        "gpu_execution": false, "metal_device_published": false,
    }))
}

fn main() {
    match run(&env::args().skip(1).collect::<Vec<_>>()) {
        Ok(value) => println!("{value}"),
        Err(error) => {
            eprintln!("REFUSED: {error}");
            process::exit(2);
        }
    }
}
