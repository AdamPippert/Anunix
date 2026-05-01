---
name: anunix-exec
description: Execute a command on a running Anunix VM on Jekyll via the HTTP API. Use when you need to run ansh commands, create tensors, inspect objects, or interact with Anunix programmatically.
argument-hint: <command>
disable-model-invocation: false
allowed-tools: Bash(ssh *) Bash(curl *)
when_to_use: run anunix command, execute on vm, tensor operation, model operation, shell command on anunix
---

# Execute Command on Anunix VM

Send a command to a running Anunix instance on Jekyll via the HTTP API.
The QEMU VM runs on Jekyll (x86_64 test target).

## Arguments

- `$ARGUMENTS` — the ansh command to execute (e.g., `sysinfo`, `tensor create default:/w 4,4 int8`)

## Steps

1. Execute via HTTP API — routed through Jekyll directly:
   ```bash
   ssh jekyll "curl -s -X POST http://localhost:18080/api/v1/exec \
     -H 'Content-Type: application/json' \
     -d '{\"command\": \"$ARGUMENTS\"}'"
   ```

2. Parse the JSON response. Format: `{"status": "ok", "output": "..."}`

3. If the response is empty or connection refused, the VM may not be running. Suggest `/anunix-deploy` first.

## Available Commands

### Object Tools
- `ls [ns:path]` — list namespace entries
- `cat <path>` — read object payload
- `write <ns:path> <content>` — create State Object
- `inspect <path>` — full object inspection
- `search <pattern>` — search payloads

### Tensor Operations (RFC-0013)
- `tensor create <ns:path> <shape> <dtype>` — create tensor (dtypes: int8, uint8, int32, float32)
- `tensor fill <ns:path> <pattern>` — fill with zeros/ones/range
- `tensor info <ns:path>` — show shape, dtype, size
- `tensor stats <ns:path>` — BRIN statistics (mean, variance, sparsity, min, max)
- `tensor slice <ns:path> <start> <end>` — row-slice
- `tensor diff <path-a> <path-b>` — element-wise delta
- `tensor quantize <ns:path> <dtype>` — dtype conversion
- `tensor search <predicate>` — find by BRIN (e.g., `sparsity>0.5`, `dtype==int8`)
- `tensor matmul <a> <b> <out>` — matrix multiply
- `tensor relu <input> [output]` — ReLU activation
- `tensor transpose <input> [output]` — transpose 2D

### Model Namespace
- `model info <name>` — show manifest
- `model layers <name>` — list layer tensors
- `model diff <a> <b>` — compare models
- `model import <ns:path> <name>` — import safetensors

### System
- `sysinfo` — CPU, memory, network, PCI
- `mem stats` — page allocator
- `netinfo` — network config
- `help` — full command list

## Chaining Commands

```bash
for cmd in "tensor create default:/a 4,4 int8" "tensor fill default:/a ones" "tensor stats default:/a"; do
  ssh jekyll "curl -s -X POST http://localhost:18080/api/v1/exec \
    -H 'Content-Type: application/json' \
    -d '{\"command\": \"$cmd\"}'" | python3 -c "import sys,json; print(json.load(sys.stdin).get('output',''))"
done
```
