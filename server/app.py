import os
import re
from datetime import datetime as dt, timezone, timedelta
from fastapi import FastAPI, HTTPException, status, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
from motor.motor_asyncio import AsyncIOMotorClient
from dotenv import load_dotenv

# ==========================================
# 1. Environment & Database Setup
# ==========================================
load_dotenv()

MONGO_URI = os.getenv("MONGO_URI")
if not MONGO_URI:
    raise ValueError("MONGO_URI is missing. Check your .env file.")

client = AsyncIOMotorClient(MONGO_URI)
db = client["simple_smart_hub"]
data_collection = db["telemetry_data"]
settings_collection = db["user_settings"]

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["https://simple-smart-hub-client.netlify.app"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ==========================================
# A. Data Contracts (Pydantic Models)
# ==========================================
class TelemetryInput(BaseModel):
    temperature: float
    presence: bool

class TelemetryDB(TelemetryInput):
    # Enforced UTC aware-to-naive standardization:
    # 1. dt.now(timezone.utc) generates a strict timezone-aware UTC timestamp.
    # 2. replace(tzinfo=None) strips the timezone signature to maintain naive format compatibility.
    # 3. replace(microsecond=0) cleans the trailing fractional seconds.
    datetime: dt = Field(
        default_factory=lambda: dt.now(timezone.utc).replace(tzinfo=None).replace(microsecond=0)
    )

class SettingsInput(BaseModel):
    user_temp: float
    user_light: str
    light_duration: str

class SettingsDB(SettingsInput):
    light_time_off: str

# ==========================================
# B. The Uplink Endpoint (Telemetry Ingestion)
# ==========================================
@app.post("/data", status_code=status.HTTP_201_CREATED)
async def ingest_telemetry(payload: TelemetryInput):
    db_payload = TelemetryDB(**payload.model_dump())

    # Standard dump ensures datetime remains a Python native type
    # so Motor stores it as a proper BSON Date in MongoDB[cite: 10].
    document = db_payload.model_dump()
    
    # Asynchronously insert into MongoDB[cite: 10, 11, 12].
    await data_collection.insert_one(document)

    # THE FIX: Pop the mutated BSON '_id' from the dictionary in RAM[cite: 2, 8].
    # Passing None prevents a KeyError if '_id' is missing[cite: 8].
    document.pop("_id", None)

    # Return the sanitized dictionary safely to the client[cite: 8].
    return document

# ==========================================
# C. The Configuration Injector
# ==========================================
@app.put("/settings", status_code=status.HTTP_200_OK)
async def update_settings(payload: SettingsInput):
    # 1. Regex parsing algorithm for time duration
    pattern = r"(\d+(?:\.\d+)?)([smhd])"
    matches = re.findall(pattern, payload.light_duration)

    if not matches:
        raise HTTPException(
            status_code=400,
            detail="Invalid light_duration format. Use formats like '4h', '2h30m'."
        )

    multipliers = {"s": 1, "m": 60, "h": 3600, "d": 86400}
    total_seconds = sum(float(val) * multipliers[unit] for val, unit in matches)

    # 2. Calculate the exact light_time_off
    try:
        start_time = dt.strptime(payload.user_light, "%H:%M:%S")
        end_time = start_time + timedelta(seconds=total_seconds)
        light_time_off = end_time.strftime("%H:%M:%S")
    except ValueError:
        raise HTTPException(
            status_code=400,
            detail="Invalid user_light format. Must be HH:MM:SS."
        )

    # 3. Singleton Database Update (wipes and replaces)
    db_settings = SettingsDB(**payload.model_dump(), light_time_off=light_time_off)
    document = db_settings.model_dump()
    await settings_collection.replace_one({}, document, upsert=True)

    # 4. Clean up response to perfectly match expected shape
    # Applying the Pop Mutation Pattern here as well to prevent leaks
    document.pop("_id", None)
    document.pop("light_duration", None)

    return document

# ==========================================
# D. The Central Decision Engine
# ==========================================
@app.get("/state", status_code=status.HTTP_200_OK)
async def get_state():
    # 1. Fetch system configurations and the latest sensor telemetry asynchronously
    settings = await settings_collection.find_one({})
    recent_data = await data_collection.find_one({}, sort=[("datetime", -1)])

    # 2. DEFENSIVE GUARD (Cold Boot Safe-Harbor)
    # If the database is empty, return a safe default to protect the hardware
    if not settings or not recent_data:
        return {"fan": False, "light": False}

    # 3. Evaluate Fan Actuation Logic (Thermodynamic Rule)
    fan_on = (recent_data["temperature"] > settings["user_temp"]) and recent_data["presence"]

    # 4. Evaluate Light Actuation Logic (Temporal Rule with Midnight Wrapping)
    # Grab current server-side UTC time formatted strictly to HH:MM:SS
    current_time_str = dt.now(timezone.utc).strftime("%H:%M:%S")
    
    start = settings["user_light"]       # Stored as UTC string "HH:MM:SS"
    end = settings["light_time_off"]     # Calculated as UTC string "HH:MM:SS"

    # Handle Time Wrapping Over Midnight
    if start <= end:
        # Standard linear window (does not cross midnight)
        in_time_window = start <= current_time_str <= end
    else:
        # Midnight-wrapping window (crosses midnight)
        in_time_window = current_time_str >= start or current_time_str <= end

    light_on = in_time_window and recent_data["presence"]

    # 5. Return the calculated actuator state payload back to the ESP32
    return {"fan": fan_on, "light": light_on}

# ==========================================
# E. The Analytics Route (Dashboard Graph)
# ==========================================
@app.get("/graph", status_code=status.HTTP_200_OK)
async def get_graph(size: int = Query(default=10, ge=1)):
    # Apply Query Projection {"_id": 0} to command the database engine 
    # to strip the _id key prior to network transmission.
    cursor = data_collection.find({}, {"_id": 0}).sort("datetime", -1).limit(size)
    
    # Asynchronously convert the cursor to a list of dictionaries
    documents = await cursor.to_list(length=size)
    
    # Restore chronological ordering for front-end React chart rendering
    documents.reverse()

    return documents