import paho.mqtt.client as mqtt
import requests
import json
import time
from datetime import datetime
import pytz

# --- Configuration ---
MQTT_BROKER = "broker.emqx.io"  # e.g., "broker.emqx.io" or your OCI IP
MQTT_PORT = 1883
MQTT_TOPIC = "/your_topic_header/ack"
MQTT_USER = None          # Leave None if not required
MQTT_PASS = None          # Leave None if not required

# WABridge Local Settings (assuming it's on the same OCI instance)
WABRIDGE_ENDPOINT = "http://localhost:8080/send" 
TARGET_PHONE = "<YOUR_WHATSAPP_NUMBER>" # Your WhatsApp number with country code

ist = pytz.timezone('Asia/Kolkata')

# --- Logic ---

def send_whatsapp_alert(message_text):
    """Sends a POST request to the local WABridge API."""
    payload = {
        "phone": TARGET_PHONE,
        "message": message_text
    }
    try:
        response = requests.post(WABRIDGE_ENDPOINT, json=payload, timeout=5)
        if response.status_code == 200:
            print("Successfully sent alert to WhatsApp.")
        else:
            print(f"WABridge error: {response.status_code} - {response.text}")
    except Exception as e:
        print(f"Failed to connect to WABridge: {e}")

def on_connect(client, userdata, flags, rc):
    """Callback for when the client connects to the broker."""
    if rc == 0:
        print(f"Connected to MQTT Broker! Subscribing to: {MQTT_TOPIC}")
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"Connection failed with code {rc}")

def on_message(client, userdata, msg):
    """Callback for when a message is received on the subscribed topic."""
    payload = msg.payload.decode()
    print(f"New MQTT Message: {payload}")

    timestamp = datetime.now(ist).strftime("%H:%M %d-%m-%Y")
    
    # check if payload is valid JSON and extract flow rate, volume, temperature, and humidity if available
    try:
        data = json.loads(payload)
        status = data.get("status", "N/A")
        flow_rate = data.get("flow_rate_lpm", "N/A")
        total_volume = data.get("total_volume_l", "N/A")
        temperature = data.get("temperature_c", "N/A")
        humidity = data.get("humidity_pct", "N/A")
        alert_text = (f"Watering completed at {timestamp}\n"
                      f"Flow Rate: {flow_rate} L/min\n"
                      f"Total Volume: {total_volume} L\n"
                      f"Temperature: {temperature} °C\n"
                      f"Humidity: {humidity} %")

    except json.JSONDecodeError:
        # If payload is not valid JSON, use a default message
        alert_text = f"Watering was completed at {timestamp}"

    if status == "OFF":
        print("Watering completed")
        send_whatsapp_alert(alert_text)

# --- Initialization ---

client = mqtt.Client()

# Set credentials if necessary
if MQTT_USER and MQTT_PASS:
    client.username_pw_set(MQTT_USER, MQTT_PASS)

client.on_connect = on_connect
client.on_message = on_message

# Connect and Loop
try:
    print("Starting MQTT Client...")
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    
    # loop_forever handles automatic reconnections
    client.loop_forever()
except KeyboardInterrupt:
    print("Exiting...")
    client.disconnect()
except Exception as e:
    print(f"An error occurred: {e}")
