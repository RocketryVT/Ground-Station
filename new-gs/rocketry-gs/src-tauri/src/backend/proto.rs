use prost::Enumeration;
use prost::Message;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Enumeration)]
#[repr(i32)]
pub enum FlightState {
    GroundIdle = 0,
    Armed = 1,
    PoweredAscent = 2,
    CoastAscent = 3,
    Apogee = 4,
    DescentDrogue = 5,
    DescentMain = 6,
    Landed = 7,
    Fault = 255,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Enumeration)]
#[repr(i32)]
pub enum JogAxis {
    Unspecified = 0,
    Az = 1,
    El = 2,
}

#[derive(Clone, PartialEq, Message)]
pub struct RocketLoRaSample {
    #[prost(uint32, optional, tag = "1")]
    pub boot_ms: Option<u32>,
    #[prost(enumeration = "FlightState", optional, tag = "2")]
    pub state: Option<i32>,
    #[prost(uint32, optional, tag = "3")]
    pub sats: Option<u32>,
    #[prost(uint32, optional, tag = "4")]
    pub flags: Option<u32>,
    #[prost(double, optional, tag = "5")]
    pub lat: Option<f64>,
    #[prost(double, optional, tag = "6")]
    pub lon: Option<f64>,
    #[prost(float, optional, tag = "7")]
    pub alt_gps_m: Option<f32>,
    #[prost(float, optional, tag = "8")]
    pub alt_baro_m: Option<f32>,
    #[prost(float, optional, tag = "9")]
    pub speed_ms: Option<f32>,
    #[prost(float, repeated, tag = "10")]
    pub q: Vec<f32>,
    #[prost(float, optional, tag = "11")]
    pub rssi: Option<f32>,
    #[prost(float, optional, tag = "12")]
    pub snr: Option<f32>,
}

#[derive(Clone, PartialEq, Message)]
pub struct Lora1Rf69Packet {
    #[prost(bytes = "vec", optional, tag = "1")]
    pub data: Option<Vec<u8>>,
    #[prost(float, optional, tag = "2")]
    pub rssi: Option<f32>,
    #[prost(float, optional, tag = "3")]
    pub snr: Option<f32>,
}

#[derive(Clone, PartialEq, Message)]
pub struct AntennaState {
    #[prost(uint64, optional, tag = "1")]
    pub timestamp: Option<u64>,
    #[prost(float, optional, tag = "2")]
    pub actual_az: Option<f32>,
    #[prost(float, optional, tag = "3")]
    pub actual_el: Option<f32>,
    #[prost(float, optional, tag = "4")]
    pub target_az: Option<f32>,
    #[prost(float, optional, tag = "5")]
    pub target_el: Option<f32>,
    #[prost(float, optional, tag = "6")]
    pub actual_az_mech: Option<f32>,
    #[prost(float, optional, tag = "7")]
    pub target_az_mech: Option<f32>,
    #[prost(bool, optional, tag = "8")]
    pub az_calibrated: Option<bool>,
    #[prost(bool, optional, tag = "9")]
    pub zen_calibrated: Option<bool>,
    #[prost(bool, optional, tag = "10")]
    pub tracking_enabled: Option<bool>,
    #[prost(bool, optional, tag = "11")]
    pub az_moving: Option<bool>,
    #[prost(bool, optional, tag = "12")]
    pub zen_moving: Option<bool>,
    #[prost(bool, optional, tag = "13")]
    pub az_faulted: Option<bool>,
    #[prost(bool, optional, tag = "14")]
    pub zen_faulted: Option<bool>,
    #[prost(string, optional, tag = "15")]
    pub mode: Option<String>,
}

#[derive(Clone, PartialEq, Message)]
pub struct GroundImu {
    #[prost(uint64, optional, tag = "1")]
    pub timestamp: Option<u64>,
    #[prost(float, optional, tag = "2")]
    pub roll: Option<f32>,
    #[prost(float, optional, tag = "3")]
    pub pitch: Option<f32>,
    #[prost(float, optional, tag = "4")]
    pub yaw: Option<f32>,
    #[prost(float, optional, tag = "5")]
    pub yaw360: Option<f32>,
    #[prost(float, repeated, tag = "6")]
    pub q: Vec<f32>,
    #[prost(float, repeated, tag = "7")]
    pub a: Vec<f32>,
    #[prost(float, repeated, tag = "8")]
    pub m: Vec<f32>,
    #[prost(bool, optional, tag = "9")]
    pub have_mag: Option<bool>,
    #[prost(bool, optional, tag = "10")]
    pub startup: Option<bool>,
    #[prost(bool, optional, tag = "11")]
    pub mag_rec: Option<bool>,
    #[prost(bool, optional, tag = "12")]
    pub acc_rec: Option<bool>,
    #[prost(float, optional, tag = "13")]
    pub alt_baro: Option<f32>,
    #[prost(float, optional, tag = "14")]
    pub temp: Option<f32>,
    #[prost(bool, optional, tag = "15")]
    pub valid: Option<bool>,
    #[prost(bool, optional, tag = "16")]
    pub have_yaw_frame: Option<bool>,
    #[prost(float, optional, tag = "17")]
    pub yaw_frame_yaw: Option<f32>,
    #[prost(float, optional, tag = "18")]
    pub yaw_frame_yaw360: Option<f32>,
    #[prost(bool, optional, tag = "19")]
    pub yaw_startup: Option<bool>,
    #[prost(float, optional, tag = "20")]
    pub bar_rel_roll: Option<f32>,
    #[prost(float, optional, tag = "21")]
    pub bar_rel_pitch: Option<f32>,
    #[prost(float, optional, tag = "22")]
    pub bar_rel_yaw: Option<f32>,
    #[prost(float, repeated, tag = "23")]
    pub bar_q: Vec<f32>,
    #[prost(float, repeated, tag = "24")]
    pub yaw_q: Vec<f32>,
    #[prost(float, repeated, tag = "25")]
    pub bar_rel_q: Vec<f32>,
}

#[derive(Clone, PartialEq, Message)]
pub struct AhrsStatus {
    #[prost(uint64, optional, tag = "1")]
    pub timestamp: Option<u64>,
    #[prost(bool, optional, tag = "2")]
    pub running: Option<bool>,
    #[prost(bool, optional, tag = "3")]
    pub have_bar_imu: Option<bool>,
    #[prost(bool, optional, tag = "4")]
    pub have_bar_mag: Option<bool>,
    #[prost(bool, optional, tag = "5")]
    pub have_yaw_imu: Option<bool>,
    #[prost(bool, optional, tag = "6")]
    pub have_yaw_mag: Option<bool>,
    #[prost(uint32, optional, tag = "7")]
    pub bar_updates: Option<u32>,
    #[prost(uint32, optional, tag = "8")]
    pub yaw_updates: Option<u32>,
}

#[derive(Clone, PartialEq, Message)]
pub struct RawImuSample {
    #[prost(uint64, optional, tag = "1")]
    pub timestamp: Option<u64>,
    #[prost(float, optional, tag = "2")]
    pub ax: Option<f32>,
    #[prost(float, optional, tag = "3")]
    pub ay: Option<f32>,
    #[prost(float, optional, tag = "4")]
    pub az: Option<f32>,
    #[prost(float, optional, tag = "5")]
    pub gx: Option<f32>,
    #[prost(float, optional, tag = "6")]
    pub gy: Option<f32>,
    #[prost(float, optional, tag = "7")]
    pub gz: Option<f32>,
    #[prost(float, optional, tag = "8")]
    pub temp: Option<f32>,
}

#[derive(Clone, PartialEq, Message)]
pub struct RawMagSample {
    #[prost(uint64, optional, tag = "1")]
    pub timestamp: Option<u64>,
    #[prost(float, optional, tag = "2")]
    pub mx: Option<f32>,
    #[prost(float, optional, tag = "3")]
    pub my: Option<f32>,
    #[prost(float, optional, tag = "4")]
    pub mz: Option<f32>,
}

#[derive(Clone, PartialEq, Message)]
pub struct RawYawImuSample {
    #[prost(uint64, optional, tag = "1")]
    pub timestamp: Option<u64>,
    #[prost(float, optional, tag = "2")]
    pub ax: Option<f32>,
    #[prost(float, optional, tag = "3")]
    pub ay: Option<f32>,
    #[prost(float, optional, tag = "4")]
    pub az: Option<f32>,
    #[prost(float, optional, tag = "5")]
    pub gx: Option<f32>,
    #[prost(float, optional, tag = "6")]
    pub gy: Option<f32>,
    #[prost(float, optional, tag = "7")]
    pub gz: Option<f32>,
    #[prost(float, optional, tag = "8")]
    pub mx_ut: Option<f32>,
    #[prost(float, optional, tag = "9")]
    pub my_ut: Option<f32>,
    #[prost(float, optional, tag = "10")]
    pub mz_ut: Option<f32>,
    #[prost(bool, optional, tag = "11")]
    pub mag_valid: Option<bool>,
    #[prost(bool, optional, tag = "12")]
    pub mag_overflow: Option<bool>,
    #[prost(float, optional, tag = "13")]
    pub temp: Option<f32>,
}

#[derive(Clone, PartialEq, Message)]
pub struct RawSensorsCommand {
    #[prost(bool, optional, tag = "1")]
    pub imu: Option<bool>,
    #[prost(bool, optional, tag = "2")]
    pub mag: Option<bool>,
    #[prost(bool, optional, tag = "3")]
    pub yaw_imu: Option<bool>,
}

#[derive(Clone, PartialEq, Message)]
pub struct AxisCommand {
    #[prost(float, optional, tag = "1")]
    pub target_angle_deg: Option<f32>,
    #[prost(float, optional, tag = "2")]
    pub speed_dps: Option<f32>,
    #[prost(bool, optional, tag = "3")]
    pub stop: Option<bool>,
}

#[derive(Clone, PartialEq, Message)]
pub struct JogCommand {
    #[prost(enumeration = "JogAxis", optional, tag = "1")]
    pub axis: Option<i32>,
    #[prost(float, optional, tag = "2")]
    pub delta_deg: Option<f32>,
    #[prost(float, optional, tag = "3")]
    pub speed_dps: Option<f32>,
}

#[derive(Clone, PartialEq, Message)]
pub struct DeclinationCommand {
    #[prost(float, optional, tag = "1")]
    pub declination_deg: Option<f32>,
}
