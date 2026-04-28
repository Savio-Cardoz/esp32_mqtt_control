#!/usr/bin/env python3
"""
Flask server for serving ESP32 firmware images
Runs on port 8800 and serves files from /esp32_images
"""

from flask import Flask, send_from_directory, jsonify
import os
from pathlib import Path

app = Flask(__name__)

# Path to ESP32 images directory
IMAGES_DIR = Path(__file__).parent / "esp32_images"

# Ensure the directory exists
IMAGES_DIR.mkdir(exist_ok=True)


@app.route('/esp32_images/', methods=['GET'])
def list_images():
    """List all available images in the directory"""
    try:
        files = os.listdir(IMAGES_DIR)
        return jsonify({
            'status': 'success',
            'files': files,
            'count': len(files)
        })
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/esp32_images/<filename>', methods=['GET'])
def download_image(filename):
    """Download a specific image file"""
    try:
        return send_from_directory(IMAGES_DIR, filename)
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 404


@app.route('/', methods=['GET'])
def index():
    """Root endpoint with server info"""
    return jsonify({
        'server': 'ESP32 Image Server',
        'port': 8800,
        'endpoints': {
            'list_images': '/esp32_images/',
            'download_image': '/esp32_images/<filename>'
        }
    })


@app.route('/health', methods=['GET'])
def health():
    """Health check endpoint"""
    return jsonify({'status': 'ok'}), 200


if __name__ == '__main__':
    print(f"Starting Flask server on port 8800...")
    print(f"Images directory: {IMAGES_DIR.absolute()}")
    print(f"Access at: http://localhost:8800")
    print(f"List images: http://localhost:8800/esp32_images/")
    app.run(host='0.0.0.0', port=8800, debug=False)
