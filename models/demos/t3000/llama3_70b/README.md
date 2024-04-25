# Llama3-70B Demo

## How to Run

### For Users on TT-VPN

If you have access to TT-VPN, you can copy the weights directly to your local machine using the following SCP commands:

1. **Copying repacked Llama3-70B weights:**
    ```bash
    scp -r 10.230.36.208:/home/llama3-data-repacked/llama-3-70b/ <repacked_output_dir>
    ```

2. **Copying Llama3-70B tokenizer:**
    ```bash
    scp -r 10.230.36.208:/home/llama3-data-repacked/tokenizer.model <path_to_checkpoint_dir>
    ```

### For Users without TT-VPN Access

If you do not have access to TT-VPN, follow these steps to download the weights directly from Meta and use the repacking script:

1. **Download the Llama3-70B weights from Meta (https://llama.meta.com/):**

2. **Repack the weights:**
    ```bash
    # This concatenates the sharded checkpoints and makes it easier for us to load.
    python models/demos/t3000/llama2_70b/scripts/repack_weights.py <path_to_checkpoint_dir> <repacked_output_dir>
    ```

### Running the Demo

After setting up the repacked weights and tokenizer, you can run the demo using the commands below:

1. **Prepare the weight cache directory:**
    ```bash
    # Make a directory for us to cache weights into. This speeds up subsequent runs.
    mkdir <weight_cache_dir>
    ```

2. **Set up environment variables:**
    ```bash
    # Make sure you export the correct llama3 weights and tokenizer paths.
    export LLAMA_CKPT_DIR=<repacked_output_dir>
    export LLAMA_TOKENIZER_PATH=<path_to_checkpoint_dir>
    export LLAMA_CACHE_PATH=<weight_cache_dir>
    ```

3. **Cache the weights (first-time setup only):**
    ```bash
    # Build a full 80 layer model to cache the weights. This will take some time.
    pytest -svv models/demos/t3000/llama2_70b/tests/test_llama_model.py::test_LlamaModel_inference[decode-8chip-T3000-80L]
    ```

4. **Run the demo:**
    ```bash
    # Run the demo using sampling decode
    pytest -svv models/demos/t3000/llama3_70b/demo/demo.py::test_LlamaModel_demo[sampling-tt-70b-80L]
    ```

## Details

- **Batch Size:** Supports batch size 32.
- **Input File:** Uses `./demo/data/multi_prompt.json`.
- **Model Configuration:** Utilizes a pretrained model.
- **Hardware Requirements:** Runs on an 8-chip T3000 machine using tensor parallelism. The host machine must have at least 512 GB of memory.
- **Model Functionality:** Implements decode-to-prefill strategy, where prompts are processed token-by-token to produce KV caches, followed by token generation in decode mode.

Ensure you follow these guidelines to successfully run the Llama3-70B demo.