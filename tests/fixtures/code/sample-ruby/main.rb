require_relative 'lib/greeter'

def run
  greeter = Greetings::Greeter.new('Ada')
  puts greeter.hello
end

run if __FILE__ == $PROGRAM_NAME
