use sample_rust::{default_config, EchoHandler, Handler, Mode};

fn main() {
    let config = default_config();
    let _mode = Mode::Development;
    let handler = EchoHandler;
    println!("listening on {}:{}", config.host, config.port);
    println!("{}", handler.handle("ping"));
}
