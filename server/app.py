import os
import re
from datetime import datetime as dt, timedelta
from uuid import UUID, uuid4
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
    id: UUID = Field(default_factory=uuid4)
    datetime: dt = Field(default_factory=dt.now)

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
    document = db_payload.model_dump(mode='json')
    
    await data_collection.insert_one(document)
    document.pop("_id", None)
    return document

# ==========================================
# C. The Configuration Injector 
# ==========================================
@app.put("/settings", status_code=status.HTTP_200_OK)
async def update_settings(payload: SettingsInput):
    # 1. Regex parsing algorithm for time duration
    pattern = r"(\d+(?:\.\d)?)([smhd])"
    matches = re.findall(pattern, payload.light_duration)
    
    if not matches:
        raise HTTPException(status_code=400, detail="Invalid light_duration format. Use formats like '4h', '2h30m'.")
        
    multipliers = {"s": 1, "m": 60, "h": 3600, "d": 86400}
    total_seconds = sum(float(val) * multipliers[unit] for val, unit in matches)
    
    # 2. Calculate the exact light_time_off
    try:
        start_time = dt.strptime(payload.user_light, "%H:%M:%S")
        end_time = start_time + timedelta(seconds=total_seconds)
        light_time_off = end_time.strftime("%H:%M:%S")
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid user_light format. Must be HH:MM:SS.")
        
    # 3. Singleton Database Update
    db_settings = SettingsDB(**payload.model_dump(), light_time_off=light_time_off)
    document = db_settings.model_dump(mode='json')
    
    await settings_collection.replace_one({}, document, upsert=True)
    document.pop("_id", None)
    return document

# ==========================================
# D. The Central Decision Engine
# ==========================================
@app.get("/state", status_code=status.HTTP_200_OK)
async def get_state():
    # 1. Fetch system state
    settings = await settings_collection.find_one({})
    if not settings:
        raise HTTPException(status_code=404, detail="System settings not configured.")
        
    recent_data = await data_collection.find_one(sort=[("datetime", -1)])
    if not recent_data:
        raise HTTPException(status_code=404, detail="No telemetry data available.")
        
    # 2. FAN LOGIC: temp > user_temp AND presence == True
    fan_on = (recent_data["temperature"] > settings["user_temp"]) and recent_data["presence"]
    
    # 3. LIGHT LOGIC: current time in window AND presence == True
    current_time_str = dt.now().strftime("%H:%M:%S")
    start = settings["user_light"]
    end = settings["light_time_off"]
    
    # Handle time wrapping over midnight
    if start <= end:
        in_time_window = start <= current_time_str <= end
    else:
        in_time_window = current_time_str >= start or current_time_str <= end
        
    light_on = in_time_window and recent_data["presence"]
    
    return {"fan": fan_on, "light": light_on}

# ==========================================
# E. The Analytics Route (Dashboard Graph)
# ==========================================
@app.get("/graph", status_code=status.HTTP_200_OK)
async def get_graph(size: int = Query(default=10, ge=1)):
    # 1. Fetch 'size' recent documents, sorted descending
    cursor = data_collection.find({}, {"_id": 0}).sort("datetime", -1).limit(size)
    documents = await cursor.to_list(length=size)
    
    # 2. Reverse array to chronological order (oldest -> newest for graphs)
    documents.reverse()
    return documents
