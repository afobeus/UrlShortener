import os

from flask import Flask, request, render_template, redirect, url_for, flash


app = Flask(__name__)
app.secret_key = os.getenv("SECRET_KEY", 'dev-default-key')


def get_short_url(original_url: str) -> str:
    return "https://example.com"

def get_original_url(code: str) -> str:
    return "https://example.com"

def normalize_url(url: str) -> str:
    if not url.startswith(("http://", "https://")):
        return "https://" + url
    return url

@app.route("/", methods=["GET"])
def index():
    return render_template("index.html")

@app.route("/shorten", methods=["POST"])
def shorten():
    original = request.form.get("url", "").strip()
    if not original:
        flash("Пожалуйста, введите URL.")
        return redirect(url_for("index"))

    original = normalize_url(original)
    short_url = get_short_url(original)
    return render_template("result.html", original=original, short_url=short_url)

@app.route("/<code>")
def redirect_short(code):
    original = get_original_url(code)
    if not original:
        return "Не найдена короткая ссылка.", 404
    return redirect(original)

if __name__ == "__main__":
    app.run()
