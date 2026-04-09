pub struct Config {
    pub port: u16,
    pub host: String,
}

pub enum Mode {
    Development,
    Production,
}

pub trait Handler {
    fn handle(&self, request: &str) -> String;
}

pub struct EchoHandler;

impl Handler for EchoHandler {
    fn handle(&self, request: &str) -> String {
        format!("echo: {}", request)
    }
}

pub fn default_config() -> Config {
    Config {
        port: 8080,
        host: String::from("localhost"),
    }
}
