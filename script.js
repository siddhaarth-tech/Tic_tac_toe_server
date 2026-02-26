// get references to html elements using id
// document.getElementById finds element in DOM
const statusDiv = document.getElementById("status");
const turnDiv = document.getElementById("turn");
const boardDiv = document.getElementById("board");
const restartBtn = document.getElementById("restart");

// websocket connection object
let socket;

// symbol assigned by server X or O
let mySymbol = null;

// true if it is my turn
let myTurn = false;

// local board state 9 cells
// Array(9).fill("") creates array of 9 empty strings
let board = Array(9).fill("");

// true if game running
let gameActive = false;


function connect() {

    // create websocket connection to server
    // ws protocol means websocket
    socket = new WebSocket("ws://localhost:8080");

    // triggered when connection established
    socket.onopen = () => {

        statusDiv.textContent = "Connected. Waiting for opponent...";

        // clear turn text until game starts
        turnDiv.textContent = "";

        // hide restart button while waiting
        restartBtn.style.display = "none";
    };

    // triggered when server sends message
    socket.onmessage = (event) => {

        // event.data contains message string
        // JSON.parse converts string to object
        const data = JSON.parse(event.data);

        // game start message
        if (data.type === "start") {

            mySymbol = data.symbol;

            // X always starts first
            myTurn = (mySymbol === "X");

            gameActive = true;

            statusDiv.textContent =
                "Game started! You are " + mySymbol;

            updateTurnUI();

            renderBoard(); 
            // End Game during active match
            restartBtn.style.display = "inline-block";
            restartBtn.textContent = "End Game";
        }

        // board update message
        if (data.type === "update") {

            // update board first
            board[data.position] = data.symbol;

            // update turn BEFORE rendering
            myTurn = (data.symbol !== mySymbol);

            updateTurnUI();

            renderBoard();
        }

        // win message
        if (data.type === "win") {

            gameActive = false;

            if (data.winner === mySymbol)
                statusDiv.textContent = "You Win!";
            else
                statusDiv.textContent = "You Lose!";

            // clear turn display after game ends
            turnDiv.textContent = "";

            // change button to New Game after match ends
            restartBtn.style.display = "inline-block";
            restartBtn.textContent = "New Game";
        }

        // draw message
        if (data.type === "draw") {

            gameActive = false;

            statusDiv.textContent = "It's a Draw!";

            turnDiv.textContent = "";

            // allow starting new game
            restartBtn.style.display = "inline-block";
            restartBtn.textContent = "New Game";
        }
    };

    // triggered when websocket closes
    socket.onclose = () => {

        // ignore unexpected close during active game
        if (gameActive) return;

        gameActive = false;

        // show button to allow reconnect
        restartBtn.style.display = "inline-block";
        restartBtn.textContent = "New Game";
    };
}


function updateTurnUI() {

    // if game not active do nothing
    if (!gameActive) return;

    if (myTurn)
        turnDiv.textContent =
            "Your Turn (" + mySymbol + ")";
    else
        turnDiv.textContent =
            "Opponent's Turn";
}

function renderBoard() {

    // clear board UI
    // innerHTML replaces all content inside element
    boardDiv.innerHTML = "";

    // forEach iterates through array
    board.forEach((cell, index) => {

        // create new div element
        const div = document.createElement("div");

        div.className = "cell";

        // if cell already has X or O
        if (cell) {

            div.textContent = cell;

            // classList.add adds css class
            div.classList.add(cell);

            // prevent clicking filled cells permanently
            div.classList.add("disabled");
        }

        // if game not active, disable everything
        if (!gameActive) {
            div.classList.add("disabled");
        }

        // if game active but NOT your turn
        if (gameActive && !myTurn) {
            div.classList.add("disabled");
        }

        // if game active AND your turn
        if (gameActive && myTurn) {

            // enable only empty cells
            if (cell === "") {
                div.classList.remove("disabled");
            }
        }

        // click event for each cell
        div.onclick = () => {

            // allow only during your turn and active game
            if (!gameActive || !myTurn) return;

            // prevent overwriting existing move
            if (board[index] !== "") return;

            // immediately disable board after selecting
            // prevents double clicking before server responds
            myTurn = false;
            renderBoard();

            // send move to server
            // JSON.stringify converts object to string
            socket.send(JSON.stringify({
                type: "move",
                position: index
            }));
        };

        // appendChild adds element to boardDiv
        boardDiv.appendChild(div);
    });
}


// triggered when restart button clicked
restartBtn.onclick = () => {

    // If game running then send exit to server
    if (gameActive && restartBtn.textContent==="End Game") {

        // notify server that player exited
        socket.send(JSON.stringify({
            type: "exit"
        }));

        gameActive = false;

        statusDiv.textContent =
            "You ended the game.";

        turnDiv.textContent = "";

        // change button to New Game
        restartBtn.textContent = "New Game";

        return;
    }

    // Start new game

    // reset local board
    board = Array(9).fill("");

    renderBoard();

    // reconnect to server
    connect();
};

//Initialised from here

// initial rendering
renderBoard();

// initial connection
connect();