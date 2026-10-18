#include "osd_schema.hpp"

namespace osd_schema {

static std::vector<Field> videoFields() {
	return {
		{"icon_path", "Icon", Kind::Icon},
		{"template", "Text", Kind::Template},
		{"per_second_window_s", "Window (s)", Kind::Int, 1, 10, 1},
		{"per_second_bucket_ms", "Bucket (ms)", Kind::Int, 10, 1000, 10},
	};
}

const std::vector<WidgetType>& types() {
	static const std::vector<WidgetType> list = {
		{"TplTextWidget", "Values",
		 {{"template", "Text", Kind::Template}},
		 0, -1, R"j({"template": "", "facts": []})j", true, false},
		{"IconTplTextWidget", "Icon + values",
		 {{"icon_path", "Icon", Kind::Icon}, {"template", "Text", Kind::Template}},
		 0, -1, R"j({"icon_path": "memory.png", "template": "", "facts": []})j", true, false},
		{"TextWidget", "Fixed text",
		 {{"text", "Text", Kind::Text}},
		 0, 0, R"j({"text": "Label", "facts": []})j", true, false},
		{"BoxWidget", "Background box",
		 {{"width", "Width", Kind::Int, 1, 1920, 5}, {"height", "Height", Kind::Int, 1, 1080, 5},
		  {"color", "Color", Kind::Color}},
		 0, 0, R"j({"width": 200, "height": 80, "color": {"r": 0.0, "g": 0.0, "b": 0.0, "alpha": 0.4}, "facts": []})j",
		 true, false},
		{"IconSelectorWidget", "Icon by value",
		 {{"ranges_and_icons", "Ranges", Kind::Ranges}},
		 1, 1, R"j({"facts": [{"name": "wfbcli.rx.ant_stats.rssi_avg", "tags": {"ant_id": "0", "id": "video rx"}}],
		  "ranges_and_icons": [
		    {"range": [-40, 1], "icon_path": "signal1.png"}, {"range": [-48, -41], "icon_path": "signal2.png"},
		    {"range": [-56, -49], "icon_path": "signal3.png"}, {"range": [-64, -57], "icon_path": "signal4.png"},
		    {"range": [-72, -65], "icon_path": "signal5.png"}, {"range": [-80, -73], "icon_path": "signal6.png"},
		    {"range": [-90, -81], "icon_path": "signal7.png"}, {"range": [-130, -91], "icon_path": "signal8.png"}]})j",
		 true, false},
		{"VideoWidget", "Video fps + size", videoFields(),
		 1, -1, R"j({"icon_path": "framerate.png", "template": "%u fps | %ux%u", "per_second_window_s": 2,
		  "per_second_bucket_ms": 200,
		  "facts": [{"name": "video.displayed_frame"}, {"name": "video.width"}, {"name": "video.height"}]})j",
		 true, false},
		{"VideoBitrateWidget", "Video bitrate", videoFields(),
		 1, 1, R"j({"icon_path": "network.png", "template": "%.2f Mbps", "per_second_window_s": 2,
		  "per_second_bucket_ms": 100, "facts": [{"name": "gstreamer.received_bytes"}]})j",
		 true, false},
		{"VideoDecodeLatencyWidget", "Video decode latency", videoFields(),
		 1, 1, R"j({"icon_path": "latency.png", "template": "%.1f ms (%d-%d)", "per_second_window_s": 2,
		  "per_second_bucket_ms": 100, "facts": [{"name": "video.decode_and_handover_ms"}]})j",
		 true, false},
		{"DvrStatusWidget", "DVR indicator",
		 {{"icon_path", "Icon", Kind::Icon}, {"text", "Text", Kind::Text}},
		 1, 1, R"j({"icon_path": "sdcard-white.png", "text": "Recording", "facts": [{"name": "dvr.recording"}]})j",
		 true, false},
		{"BatteryCellWidget", "Battery cell",
		 {{"template", "Text", Kind::Template},
		  {"critical_voltage", "Critical (V)", Kind::Double, 2.5, 4.5, 0.05, 2, nullptr, 3.5},
		  {"max_voltage", "Full (V)", Kind::Double, 3.0, 4.5, 0.05, 2, nullptr, 4.2},
		  {"num_cells", "Cells", Kind::Enum, 0, 0, 1, 0, "auto\neven\n1\n2\n3\n4\n5\n6\n7\n8\n10\n12"}},
		 1, 1, R"j({"template": "%.2fV", "critical_voltage": 3.5, "max_voltage": 4.2, "num_cells": "even",
		  "facts": [{"name": "mavlink.sys_status.voltage_battery"}]})j",
		 true, false},
		{"GPSWidget", "GPS position",
		 {},
		 3, 3, R"j({"facts": [{"name": "mavlink.gps_raw.fix_type"}, {"name": "mavlink.gps_raw.lat"},
		  {"name": "mavlink.gps_raw.lon"}]})j",
		 true, false},
		{"BarChartWidget", "Bar chart",
		 {{"width", "Width", Kind::Int, 10, 1920, 10}, {"height", "Height", Kind::Int, 10, 1080, 10},
		  {"window_s", "Window (s)", Kind::Int, 1, 300, 1}, {"num_buckets", "Bars", Kind::Int, 1, 200, 1},
		  {"stats_kind", "Value", Kind::Enum, 0, 0, 1, 0, "sum\navg\nmin\nmax\ncount"}},
		 1, 1, R"j({"width": 300, "height": 100, "window_s": 10, "num_buckets": 20, "stats_kind": "sum",
		  "facts": [{"name": "gstreamer.received_bytes"}]})j",
		 true, false},
		{"PopupWidget", "Pop-up messages",
		 {{"timeout_ms", "Show for (ms)", Kind::Int, 500, 60000, 500}},
		 0, -1, R"j({"timeout_ms": 10000, "facts": [{"name": "osd.custom_message"}]})j",
		 true, false},
		{"SignalWarningWidget", "Signal warning",
		 {{"threshold", "Warn below/above", Kind::Number}, {"critical", "Full at", Kind::Number},
		  {"average_ms", "Smoothing (ms)", Kind::Int, 0, 10000, 100},
		  {"rate", "Per-second rate", Kind::Bool},
		  {"rate_divisor", "Rate divisor", Kind::Number, 0, 0, 1, 0, nullptr, 1}},
		 1, -1, R"j({"rate": true, "rate_divisor": 125000, "average_ms": 1000, "threshold": 8, "critical": 2,
		  "facts": [{"name": "gstreamer.received_bytes"}]})j",
		 false, false},
		{"DebugWidget", "Debug (raw facts)",
		 {},
		 0, -1, R"j({"facts": []})j", true, false},
		{"ExternalSurfaceWidget", "External surface",
		 {{"shm_name", "Shared memory", Kind::Text},
		  {"refresh_interval_ms", "Refresh (ms)", Kind::Int, 5, 1000, 1, 0, nullptr, 16}},
		 0, 0, R"j({"shm_name": "msposd", "refresh_interval_ms": 16, "facts": []})j", true, true},
		{"MspDisplayPortWidget", "MSP DisplayPort",
		 {{"font_path", "Font file", Kind::Text},
		  {"udp_port", "UDP port", Kind::Number, 1, 65535, 1, 0, nullptr, 14551}},
		 0, 0, R"j({"font_path": "/usr/share/fonts/font_btfl_hd.png", "udp_port": 14551, "facts": []})j", true, true},
	};
	return list;
}

const WidgetType* findType(const std::string& type) {
	for (const auto& t : types())
		if (type == t.type) return &t;
	return nullptr;
}

static void add(std::vector<FactInfo>& v, std::string name, const char* type, const char* desc,
                std::vector<std::string> tags = {}) {
	v.push_back({std::move(name), type, desc, std::move(tags)});
}

const std::vector<FactInfo>& catalog() {
	static const std::vector<FactInfo> list = [] {
		std::vector<FactInfo> v;
		// Video / decoder (main.cpp)
		add(v, "video.displayed_frame", "UINT", "1 per shown frame (VideoWidget makes it fps)");
		add(v, "video.width", "UINT", "Video width, px");
		add(v, "video.height", "UINT", "Video height, px");
		add(v, "video.codec", "STRING", "h264 / h265");
		add(v, "video.codec_id", "UINT", "0 = h264, 1 = h265");
		add(v, "video.decode_and_handover_ms", "UINT", "Decode + display time, ms");
		add(v, "video.decoder_feed_time_ms", "UINT", "Time to feed the decoder (when retrying), ms");
		add(v, "gstreamer.received_bytes", "UINT", "Bytes per received packet (bitrate widgets)");
		add(v, "dvr.recording", "BOOL", "DVR is recording");
		add(v, "osd.custom_message", "STRING", "Custom message (/run/pixelpilot.msg)", {"file"});

		// WFB-ng (wfbcli.cpp)
		const std::vector<std::string> ant = {"id", "ant_id"};
		add(v, "wfbcli.rx.ant_stats.rssi_avg", "INT", "RSSI average, dBm", ant);
		add(v, "wfbcli.rx.ant_stats.rssi_min", "INT", "RSSI minimum, dBm", ant);
		add(v, "wfbcli.rx.ant_stats.rssi_max", "INT", "RSSI maximum, dBm", ant);
		add(v, "wfbcli.rx.ant_stats.snr_avg", "INT", "SNR average, dB", ant);
		add(v, "wfbcli.rx.ant_stats.snr_min", "INT", "SNR minimum, dB", ant);
		add(v, "wfbcli.rx.ant_stats.snr_max", "INT", "SNR maximum, dB", ant);
		add(v, "wfbcli.rx.ant_stats.mcs", "UINT", "MCS index", ant);
		add(v, "wfbcli.rx.ant_stats.freq", "UINT", "Frequency, MHz", ant);
		add(v, "wfbcli.rx.ant_stats.bw", "UINT", "Bandwidth, MHz", ant);
		add(v, "wfbcli.rx.ant_stats.pkt_recv", "UINT", "Packets received (per interval)", ant);
		for (const char* key : {"all", "all_bytes", "dec_err", "session", "data", "uniq", "fec_rec",
		                        "lost", "bad", "out", "out_bytes"}) {
			add(v, std::string("wfbcli.rx.packets.") + key + ".delta", "UINT",
			    "RX packets per interval", {"id"});
			add(v, std::string("wfbcli.rx.packets.") + key + ".total", "UINT", "RX packets total", {"id"});
		}
		for (const char* key : {"incoming", "incoming_bytes", "injected", "injected_bytes", "dropped",
		                        "truncated", "fec_timeouts"}) {
			add(v, std::string("wfbcli.tx.packets.") + key + ".delta", "UINT",
			    "TX packets per interval", {"id"});
			add(v, std::string("wfbcli.tx.packets.") + key + ".total", "UINT", "TX packets total", {"id"});
		}
		add(v, "wfbcli.rf_temperature", "UINT", "Adapter RF temperature, C", ant);
		add(v, "wfbcli.temp_overheat_warning", "UINT", "Overheat warning level");
		add(v, "wfbcli.cli_title", "STRING", "wfb-ng profile title");
		add(v, "wfbcli.is_cluster", "BOOL", "wfb-ng runs as a cluster");

		// APFPV WiFi (WiFiMonitor.cpp)
		add(v, "os_mon.wifi.rssi", "INT", "WiFi adapter RSSI, dBm", {"interface", "adapter", "type"});
		add(v, "os_mon.wifi.temperature", "INT", "WiFi adapter temperature, C",
		    {"interface", "adapter", "rf_path"});

		// Ground station (os_mon.cpp)
		add(v, "os_mon.cpu.load_total", "UINT", "CPU load, %");
		add(v, "os_mon.cpu.load_iowait", "UINT", "CPU IO wait, %");
		add(v, "os_mon.temperature", "INT", "Temperature, 1/1000 C (convert: x / 1000)", {"sensor", "name"});
		add(v, "os_mon.power.voltage", "INT", "Supply voltage, mV", {"sensor"});
		add(v, "os_mon.power.current", "INT", "Supply current, mA", {"sensor"});
		add(v, "os_mon.power.power", "INT", "Supply power, uW", {"sensor"});

		// Drone telemetry (mavlink.c)
		const std::vector<std::string> mav = {"sysid", "compid"};
		add(v, "mavlink.heartbeet.base_mode.armed", "BOOL", "Armed", mav);
		add(v, "mavlink.sys_status.voltage_battery", "UINT", "Battery voltage, mV", mav);
		add(v, "mavlink.sys_status.current_battery", "INT", "Battery current, 10 mA", mav);
		add(v, "mavlink.battery_status.current_consumed", "INT", "Consumed, mAh",
		    {"sysid", "compid", "battery_id"});
		add(v, "mavlink.battery_status.energy_consumed", "INT", "Consumed, hJ",
		    {"sysid", "compid", "battery_id"});
		add(v, "mavlink.vfr_hud.airspeed", "DOUBLE", "Airspeed, m/s", mav);
		add(v, "mavlink.vfr_hud.groundspeed", "DOUBLE", "Ground speed, m/s", mav);
		add(v, "mavlink.vfr_hud.alt", "DOUBLE", "Altitude (MSL), m", mav);
		add(v, "mavlink.vfr_hud.climb", "DOUBLE", "Climb rate, m/s", mav);
		add(v, "mavlink.vfr_hud.heading", "INT", "Heading, deg", mav);
		add(v, "mavlink.vfr_hud.throttle", "UINT", "Throttle, %", mav);
		add(v, "mavlink.attitude.roll", "DOUBLE", "Roll, rad", mav);
		add(v, "mavlink.attitude.pitch", "DOUBLE", "Pitch, rad", mav);
		add(v, "mavlink.attitude.yaw", "DOUBLE", "Yaw, rad", mav);
		add(v, "mavlink.attitude.rollspeed", "DOUBLE", "Roll rate, rad/s", mav);
		add(v, "mavlink.attitude.pitchpeed", "DOUBLE", "Pitch rate, rad/s", mav);
		add(v, "mavlink.attitude.yawspeed", "DOUBLE", "Yaw rate, rad/s", mav);
		add(v, "mavlink.gps_raw.fix_type", "UINT", "GPS fix type", mav);
		add(v, "mavlink.gps_raw.satellites_visible", "UINT", "Satellites", mav);
		add(v, "mavlink.gps_raw.lat", "INT", "Latitude, deg x 1e7", mav);
		add(v, "mavlink.gps_raw.lon", "INT", "Longitude, deg x 1e7", mav);
		add(v, "mavlink.gps_raw.alt", "INT", "GPS altitude, mm", mav);
		add(v, "mavlink.gps_raw.vel", "UINT", "GPS speed, cm/s", mav);
		add(v, "mavlink.gps_raw.cog", "UINT", "Course over ground, cdeg", mav);
		add(v, "mavlink.global_position_int.lat", "INT", "Latitude, deg x 1e7", mav);
		add(v, "mavlink.global_position_int.lon", "INT", "Longitude, deg x 1e7", mav);
		add(v, "mavlink.global_position_int.alt", "INT", "Altitude (MSL), mm", mav);
		add(v, "mavlink.global_position_int.relative_alt", "INT", "Altitude above home, mm", mav);
		add(v, "mavlink.global_position_int.vx", "INT", "Speed north, cm/s", mav);
		add(v, "mavlink.global_position_int.vy", "INT", "Speed east, cm/s", mav);
		add(v, "mavlink.global_position_int.vz", "INT", "Speed down, cm/s", mav);
		add(v, "mavlink.global_position_int.hdg", "UINT", "Heading, cdeg", mav);
		for (int ch = 1; ch <= 8; ch++)
			add(v, "mavlink.rc_channels_raw.chan" + std::to_string(ch), "UINT", "RC channel, us", mav);
		add(v, "mavlink.radio_status.rssi", "INT", "Telemetry radio RSSI", mav);
		add(v, "mavlink.radio_status.remrssi", "INT", "Remote radio RSSI", mav);
		add(v, "mavlink.radio_status.noise", "INT", "Radio noise", mav);
		add(v, "mavlink.radio_status.remnoise", "UINT", "Remote radio noise", mav);
		add(v, "mavlink.radio_status.rxerrors", "UINT", "Radio RX errors", mav);
		add(v, "mavlink.radio_status.fixed", "UINT", "Radio corrected packets", mav);
		add(v, "mavlink.calculated.radio_status.snr", "INT", "Radio SNR", mav);
		for (const char* axis : {"xacc", "yacc", "zacc", "xgyro", "ygyro", "zgyro", "temperature"})
			add(v, std::string("mavlink.raw_imu.") + axis, "INT", "Raw IMU", {"sysid", "compid", "imu_id"});

		add(v, "test.value", "INT", "Test value (+/- keys)");
		return v;
	}();
	return list;
}

const FactInfo* findFact(const std::string& name) {
	for (const auto& f : catalog())
		if (f.name == name) return &f;
	return nullptr;
}

static const std::vector<std::pair<std::string, std::string>>& prefixes() {
	static const std::vector<std::pair<std::string, std::string>> list = {
		{"video.", "Video"},
		{"gstreamer.", "Video"},
		{"dvr.", "DVR & messages"},
		{"osd.", "DVR & messages"},
		{"wfbcli.", "WFB-ng link"},
		{"os_mon.wifi.", "APFPV WiFi"},
		{"os_mon.", "Ground station"},
		{"mavlink.", "Drone telemetry"},
	};
	return list;
}

std::string categoryOf(const std::string& fact_name) {
	for (const auto& [prefix, category] : prefixes())
		if (fact_name.rfind(prefix, 0) == 0) return category;
	return "Other";
}

const std::vector<std::string>& categories() {
	static const std::vector<std::string> list = {
		"Video", "WFB-ng link", "APFPV WiFi", "Ground station", "Drone telemetry", "DVR & messages", "Other",
	};
	return list;
}

std::string placeholderFor(const std::string& type) {
	if (type == "BOOL") return "%b";
	if (type == "INT") return "%d";
	if (type == "UINT") return "%u";
	if (type == "DOUBLE") return "%.2f";
	return "%s";
}

}  // namespace osd_schema
