use std::io;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::signal;
use tokio::task;

async fn handle_client(mut socket: TcpStream) -> io::Result<()> {
    let mut buffer = vec![0u8; 8192];

    loop {
        // Read from the client
        let n = socket.read(&mut buffer).await?;

        if n == 0 {
            // Connection closed
            println!("Client disconnected");
            return Ok(());
        }

        // Echo response (simplified HTTP response like in C++ version)
        let response = b"HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";

        // Write response
        socket.write_all(response).await?;
    }
}

async fn accept_loop(listener: TcpListener) -> io::Result<()> {
    println!("Worker accepting connections");

    loop {
        // Accept connection
        let (socket, addr) = listener.accept().await?;
        println!("Accepted connection from: {}", addr);

        // Spawn a task to handle this client
        task::spawn(async move {
            if let Err(e) = handle_client(socket).await {
                eprintln!("Error handling client: {}", e);
            }
        });
    }
}

#[tokio::main(flavor = "multi_thread", worker_threads = 4)]
async fn main() -> io::Result<()> {
    println!("Starting TCP echo server...");

    // Create listener with SO_REUSEADDR and SO_REUSEPORT
    let listener = TcpListener::bind("0.0.0.0:8080").await?;
    println!("Listening on port 8080");

    // Spawn accept loop
    let accept_task = task::spawn(accept_loop(listener));

    println!("Server running with 4 workers. Press Ctrl+C to stop.");

    // Wait for Ctrl+C signal
    signal::ctrl_c().await?;
    println!("Shutting down server...");

    // Cancel the accept task
    accept_task.abort();

    println!("Server stopped");
    Ok(())
}